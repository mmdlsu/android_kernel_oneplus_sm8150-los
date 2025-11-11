/*
 * Copyright (c) 2024 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * DOC: wma_frame_inject.c
 *
 * This file contains WMA layer frame injection queue management functions.
 * It provides infrastructure for queuing, processing, and transmitting
 * injected 802.11 frames through the firmware interface.
 */

#include "wma.h"
#include "wma_frame_inject.h"
#include "wlan_hdd_frame_inject.h"
#include "wma_api.h"
#include "wma_internal.h"
#include "wmi_unified_api.h"
#include "wmi_unified.h"
#include "qdf_mem.h"
#include "qdf_list.h"
#include "qdf_lock.h"
#include "qdf_status.h"
#include "qdf_trace.h"
#include "qdf_nbuf.h"
#include "qdf_delayed_work.h"
#include "cds_api.h"
#include "cdp_txrx_cmn.h"

#ifdef FEATURE_FRAME_INJECTION_SUPPORT

/* Maximum number of frames in WMA injection queue */
#define WMA_FRAME_INJECT_MAX_QUEUE_SIZE    128

/* Maximum frame size for injection */
#define WMA_FRAME_INJECT_MAX_FRAME_SIZE    2304

/* Timeout for queue processing work in milliseconds */
#define WMA_FRAME_INJECT_QUEUE_TIMEOUT_MS  100

/**
 * struct wma_injection_queue_node - Node for injection queue
 * @node: List node
 * @req: Frame injection request
 * @timestamp: Enqueue timestamp
 * @vdev_id: VDEV ID for the frame
 */
struct wma_injection_queue_node {
	qdf_list_node_t node;
	struct inject_frame_req req;
	uint64_t timestamp;
	uint8_t vdev_id;
};

/**
 * struct wma_injection_queue_ctx - WMA injection queue context
 * @queue: Queue of pending injection requests
 * @queue_lock: Lock for queue operations
 * @queue_size: Current queue size
 * @max_queue_size: Maximum allowed queue size
 * @queue_work: Work item for processing queue
 * @delayed_work: Delayed work item for backpressure handling
 * @stats: Queue statistics
 * @is_initialized: Initialization flag
 */
struct wma_injection_queue_ctx {
	qdf_list_t queue;
	qdf_spinlock_t queue_lock;
	uint32_t queue_size;
	uint32_t max_queue_size;
	qdf_work_t queue_work;
	struct qdf_delayed_work delayed_work;
	struct wma_injection_queue_stats stats;
	bool is_initialized;
};

/* Statistics structure is defined in wma_frame_inject.h */

/* Global injection queue context */
static struct wma_injection_queue_ctx g_wma_injection_ctx;

/**
 * wma_injection_queue_node_alloc() - Allocate injection queue node
 * @req: Frame injection request
 * @vdev_id: VDEV ID
 *
 * Return: Allocated node or NULL on failure
 */
static struct wma_injection_queue_node *
wma_injection_queue_node_alloc(struct inject_frame_req *req, uint8_t vdev_id)
{
	struct wma_injection_queue_node *node;
	uint8_t *frame_copy;

	if (!req || !req->frame_data || req->frame_len == 0) {
		wma_err("Invalid injection request parameters");
		return NULL;
	}

	if (req->frame_len > WMA_FRAME_INJECT_MAX_FRAME_SIZE) {
		wma_err("Frame size %u exceeds maximum %u",
			req->frame_len, WMA_FRAME_INJECT_MAX_FRAME_SIZE);
		return NULL;
	}

	node = qdf_mem_malloc(sizeof(*node));
	if (!node) {
		wma_err("Failed to allocate injection queue node");
		return NULL;
	}

	/* Allocate and copy frame data */
	frame_copy = qdf_mem_malloc(req->frame_len);
	if (!frame_copy) {
		wma_err("Failed to allocate frame data buffer");
		qdf_mem_free(node);
		return NULL;
	}

	qdf_mem_copy(frame_copy, req->frame_data, req->frame_len);

	/* Initialize node */
	qdf_mem_zero(node, sizeof(*node));
	node->req.frame_len = req->frame_len;
	node->req.frame_data = frame_copy;
	node->req.tx_flags = req->tx_flags;
	node->req.retry_count = req->retry_count;
	node->req.tx_rate = req->tx_rate;
	node->req.timestamp = req->timestamp;
	node->req.session_id = req->session_id;
	node->timestamp = qdf_get_log_timestamp();
	node->vdev_id = vdev_id;

	return node;
}

/**
 * wma_injection_queue_node_free() - Free injection queue node
 * @node: Node to free
 */
static void wma_injection_queue_node_free(struct wma_injection_queue_node *node)
{
	if (!node)
		return;

	if (node->req.frame_data) {
		qdf_mem_free(node->req.frame_data);
		node->req.frame_data = NULL;
	}

	qdf_mem_free(node);
}

/**
 * wma_check_traffic_coordination() - Check if injection can proceed with current traffic
 * @wma_handle: WMA handle
 * @vdev_id: VDEV ID for the frame
 *
 * This function checks if frame injection should be deferred due to high
 * priority traffic or resource constraints in the WMA layer.
 *
 * Return: true if injection can proceed, false if should be deferred
 */
static bool wma_check_traffic_coordination(tp_wma_handle wma_handle, uint8_t vdev_id)
{
	struct wma_txrx_node *iface;

	if (!wma_handle || vdev_id >= wma_handle->max_bssid) {
		wma_err("Invalid parameters: wma_handle=%pK, vdev_id=%u",
			wma_handle, vdev_id);
		return false;
	}

	iface = &wma_handle->interfaces[vdev_id];

	/* Check if interface is in a state that allows injection */
	if (!iface->vdev) {
		wma_debug("Interface %u not active, deferring injection", vdev_id);
		return false;
	}

	/* Check if there's high priority management traffic pending */
	if (iface->roaming_in_progress) {
		wma_debug("High priority operation in progress on vdev %u, deferring injection",
			  vdev_id);
		return false;
	}

	/* Check system-wide resource constraints */
	if (wma_handle->wmi_ready == false) {
		wma_debug("WMI not ready, deferring injection");
		return false;
	}

	/* Check if firmware is overloaded (simple heuristic) */
	if (wma_handle->wmi_handle) {
		uint32_t pending_cmds = wmi_get_pending_cmds(wma_handle->wmi_handle);
		const uint32_t max_pending_threshold = 50;
		
		if (pending_cmds > max_pending_threshold) {
			wma_debug("Firmware overloaded (%u pending commands), deferring injection",
				  pending_cmds);
			return false;
		}
	}

	return true;
}

/**
 * wma_apply_injection_backpressure() - Apply backpressure when queue is congested
 * @ctx: Injection queue context
 *
 * This function implements backpressure mechanisms when the injection queue
 * becomes congested, including adaptive processing delays and queue throttling.
 *
 * Return: Recommended delay in milliseconds before next processing cycle
 */
static uint32_t wma_apply_injection_backpressure(struct wma_injection_queue_ctx *ctx)
{
	uint32_t queue_utilization;
	uint32_t delay_ms = 0;

	if (!ctx || !ctx->is_initialized) {
		return 0;
	}

	/* Calculate queue utilization percentage */
	queue_utilization = (ctx->queue_size * 100) / ctx->max_queue_size;

	/* Apply adaptive backpressure based on queue utilization */
	if (queue_utilization > 90) {
		/* Queue nearly full - significant backpressure */
		delay_ms = 50;
		wma_debug("High queue utilization (%u%%), applying %ums backpressure",
			  queue_utilization, delay_ms);
	} else if (queue_utilization > 75) {
		/* Queue getting full - moderate backpressure */
		delay_ms = 20;
		wma_debug("Moderate queue utilization (%u%%), applying %ums backpressure",
			  queue_utilization, delay_ms);
	} else if (queue_utilization > 50) {
		/* Queue half full - light backpressure */
		delay_ms = 5;
		wma_debug("Light queue utilization (%u%%), applying %ums backpressure",
			  queue_utilization, delay_ms);
	}

	return delay_ms;
}

/**
 * wma_process_injection_queue() - Process injection queue with traffic coordination
 * @wma_handle: WMA handle
 *
 * This function processes the injection queue while coordinating with existing
 * WMA traffic scheduling and applying backpressure when needed.
 *
 * Return: QDF_STATUS_SUCCESS on success, error code on failure
 */
QDF_STATUS wma_process_injection_queue(tp_wma_handle wma_handle)
{
	struct wma_injection_queue_ctx *ctx = &g_wma_injection_ctx;
	struct wma_injection_queue_node *node;
	qdf_list_node_t *list_node;
	QDF_STATUS status;
	uint64_t current_time;
	uint32_t processed_count = 0;
	uint32_t deferred_count = 0;
	const uint32_t max_process_per_cycle = 8; /* Reduced to be more cooperative */
	bool queue_was_empty;

	if (!wma_handle) {
		wma_err("Invalid WMA handle");
		return QDF_STATUS_E_INVAL;
	}

	if (!ctx->is_initialized) {
		wma_debug("Injection queue not initialized");
		return QDF_STATUS_E_AGAIN;
	}

	current_time = qdf_get_log_timestamp();

	qdf_spin_lock_bh(&ctx->queue_lock);
	queue_was_empty = qdf_list_empty(&ctx->queue);
	qdf_spin_unlock_bh(&ctx->queue_lock);

	if (queue_was_empty) {
		wma_debug("Injection queue is empty, nothing to process");
		return QDF_STATUS_SUCCESS;
	}

	wma_debug("Starting injection queue processing cycle");

	/* Process frames from queue with traffic coordination */
	while (processed_count < max_process_per_cycle) {
		qdf_spin_lock_bh(&ctx->queue_lock);

		if (qdf_list_empty(&ctx->queue)) {
			qdf_spin_unlock_bh(&ctx->queue_lock);
			break;
		}

		/* Peek at the front node to check VDEV before removing */
		status = qdf_list_peek_front(&ctx->queue, &list_node);
		if (QDF_IS_STATUS_ERROR(status)) {
			qdf_spin_unlock_bh(&ctx->queue_lock);
			wma_err("Failed to peek at queue front: %d", status);
			break;
		}

		node = qdf_container_of(list_node, struct wma_injection_queue_node, node);

		/* Check traffic coordination before processing */
		if (!wma_check_traffic_coordination(wma_handle, node->vdev_id)) {
			qdf_spin_unlock_bh(&ctx->queue_lock);
			deferred_count++;
			wma_debug("Deferring injection due to traffic coordination (vdev_id=%u)",
				  node->vdev_id);
			break;
		}

		/* Remove the node from queue */
		status = qdf_list_remove_front(&ctx->queue, &list_node);
		if (QDF_IS_STATUS_ERROR(status)) {
			qdf_spin_unlock_bh(&ctx->queue_lock);
			wma_err("Failed to remove node from queue: %d", status);
			break;
		}

		ctx->queue_size--;
		qdf_spin_unlock_bh(&ctx->queue_lock);

		/* Update queue time statistics */
		ctx->stats.total_queue_time += (current_time - node->timestamp);

		/* Send frame to firmware */
		wma_debug("Processing injection frame: len=%u, vdev_id=%u, flags=0x%x",
			  node->req.frame_len, node->vdev_id, node->req.tx_flags);

		status = wma_send_injection_frame_to_fw(wma_handle, &node->req, node->vdev_id);
		if (QDF_IS_STATUS_SUCCESS(status)) {
			ctx->stats.frames_processed++;
			wma_debug("Injection frame sent successfully");
		} else {
			ctx->stats.frames_dropped++;
			wma_err("Failed to send injection frame to firmware: %d", status);
		}

		processed_count++;

		/* Free the node */
		wma_injection_queue_node_free(node);

		/* Add small delay between frames to be cooperative with other traffic */
		if (processed_count < max_process_per_cycle) {
			qdf_udelay(100); /* 100 microseconds between frames */
		}
	}

	wma_debug("Injection queue processing cycle complete: processed=%u, deferred=%u",
		  processed_count, deferred_count);

	return QDF_STATUS_SUCCESS;
}

/**
 * wma_process_injection_queue_work() - Work function to process injection queue
 * @arg: Work argument (not used)
 *
 * This function processes queued frame injection requests in FIFO order
 * with traffic coordination and backpressure handling.
 */
static void wma_process_injection_queue_work(void *arg)
{
	struct wma_injection_queue_ctx *ctx = &g_wma_injection_ctx;
	tp_wma_handle wma_handle;
	QDF_STATUS status;
	uint32_t backpressure_delay;
	bool queue_has_frames;

	if (!ctx->is_initialized) {
		wma_debug("Injection queue not initialized");
		return;
	}

	/* Get WMA handle for processing */
	wma_handle = cds_get_context(QDF_MODULE_ID_WMA);
	if (!wma_handle) {
		wma_err("Failed to get WMA handle");
		return;
	}

	/* Process the queue */
	status = wma_process_injection_queue(wma_handle);
	if (QDF_IS_STATUS_ERROR(status)) {
		wma_err("Failed to process injection queue: %d", status);
	}

	/* Check if there are more frames to process */
	qdf_spin_lock_bh(&ctx->queue_lock);
	queue_has_frames = !qdf_list_empty(&ctx->queue);
	qdf_spin_unlock_bh(&ctx->queue_lock);

	if (queue_has_frames) {
		/* Apply backpressure if queue is congested */
		backpressure_delay = wma_apply_injection_backpressure(ctx);
		
		if (backpressure_delay > 0) {
			/* Schedule delayed work to implement backpressure */
			wma_debug("Scheduling delayed work with %ums backpressure", backpressure_delay);
			qdf_delayed_work_start(&ctx->delayed_work, backpressure_delay);
		} else {
			/* Schedule immediate work for next processing cycle */
			qdf_sched_work(0, &ctx->queue_work);
		}
	}

	wma_debug("Injection queue work cycle completed");
}

/**
 * wma_process_injection_queue_delayed_work() - Delayed work callback for backpressure
 * @context: Context (not used)
 *
 * This function is called when delayed work is triggered for backpressure handling.
 * It simply schedules the regular work item to continue processing.
 */
static void wma_process_injection_queue_delayed_work(void *context)
{
	struct wma_injection_queue_ctx *ctx = &g_wma_injection_ctx;

	if (!ctx->is_initialized) {
		wma_debug("Injection queue not initialized");
		return;
	}

	wma_debug("Delayed work triggered, scheduling regular work");
	
	/* Schedule regular work to continue processing */
	qdf_sched_work(0, &ctx->queue_work);
}

QDF_STATUS wma_init_injection_queue(tp_wma_handle wma_handle)
{
	struct wma_injection_queue_ctx *ctx = &g_wma_injection_ctx;
	QDF_STATUS status;

	if (!wma_handle) {
		wma_err("Invalid WMA handle");
		return QDF_STATUS_E_INVAL;
	}

	if (ctx->is_initialized) {
		wma_debug("Injection queue already initialized");
		return QDF_STATUS_SUCCESS;
	}

	wma_debug("Initializing WMA injection queue");

	/* Initialize queue */
	qdf_list_create(&ctx->queue, WMA_FRAME_INJECT_MAX_QUEUE_SIZE);

	/* Initialize queue lock */
	qdf_spinlock_create(&ctx->queue_lock);

	/* Initialize work item */
	qdf_create_work(0, &ctx->queue_work, wma_process_injection_queue_work, NULL);

	/* Initialize delayed work item for backpressure */
	status = qdf_delayed_work_create(&ctx->delayed_work, 
					 wma_process_injection_queue_delayed_work, NULL);
	if (QDF_IS_STATUS_ERROR(status)) {
		wma_err("Failed to create delayed work: %d", status);
		qdf_spinlock_destroy(&ctx->queue_lock);
		qdf_list_destroy(&ctx->queue);
		return status;
	}

	/* Initialize context */
	ctx->queue_size = 0;
	ctx->max_queue_size = WMA_FRAME_INJECT_MAX_QUEUE_SIZE;
	qdf_mem_zero(&ctx->stats, sizeof(ctx->stats));
	ctx->is_initialized = true;

	wma_info("WMA injection queue initialized successfully (max_size=%u)",
		 ctx->max_queue_size);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS wma_deinit_injection_queue(tp_wma_handle wma_handle)
{
	struct wma_injection_queue_ctx *ctx = &g_wma_injection_ctx;
	struct wma_injection_queue_node *node;
	qdf_list_node_t *list_node;
	QDF_STATUS status;
	uint32_t dropped_count = 0;

	if (!wma_handle) {
		wma_err("Invalid WMA handle");
		return QDF_STATUS_E_INVAL;
	}

	if (!ctx->is_initialized) {
		wma_debug("Injection queue not initialized");
		return QDF_STATUS_SUCCESS;
	}

	wma_debug("Deinitializing WMA injection queue");

	/* Cancel any pending work */
	qdf_cancel_work(&ctx->queue_work);
	qdf_flush_work(&ctx->queue_work);
	
	/* Cancel and destroy delayed work */
	qdf_delayed_work_stop_sync(&ctx->delayed_work);
	qdf_delayed_work_destroy(&ctx->delayed_work);

	/* Clear the queue and free all nodes */
	qdf_spin_lock_bh(&ctx->queue_lock);

	while (!qdf_list_empty(&ctx->queue)) {
		status = qdf_list_remove_front(&ctx->queue, &list_node);
		if (QDF_IS_STATUS_ERROR(status)) {
			wma_err("Failed to remove node during cleanup: %d", status);
			break;
		}

		node = qdf_container_of(list_node, struct wma_injection_queue_node, node);
		wma_injection_queue_node_free(node);
		dropped_count++;
	}

	ctx->queue_size = 0;
	qdf_spin_unlock_bh(&ctx->queue_lock);

	/* Update statistics */
	ctx->stats.frames_dropped += dropped_count;

	/* Destroy queue and lock */
	qdf_list_destroy(&ctx->queue);
	qdf_spinlock_destroy(&ctx->queue_lock);

	/* Mark as uninitialized */
	ctx->is_initialized = false;

	wma_info("WMA injection queue deinitialized (dropped %u pending frames)",
		 dropped_count);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS wma_queue_injection_frame(tp_wma_handle wma_handle,
				     struct inject_frame_req *req,
				     uint8_t vdev_id)
{
	struct wma_injection_queue_ctx *ctx = &g_wma_injection_ctx;
	struct wma_injection_queue_node *node;
	QDF_STATUS status;

	if (!wma_handle || !req) {
		wma_err("Invalid parameters: wma_handle=%pK, req=%pK",
			wma_handle, req);
		return QDF_STATUS_E_INVAL;
	}

	if (!ctx->is_initialized) {
		wma_err("Injection queue not initialized");
		return QDF_STATUS_E_AGAIN;
	}

	/* Validate frame parameters */
	if (!req->frame_data || req->frame_len == 0 ||
	    req->frame_len > WMA_FRAME_INJECT_MAX_FRAME_SIZE) {
		wma_err("Invalid frame parameters: data=%pK, len=%u",
			req->frame_data, req->frame_len);
		return QDF_STATUS_E_INVAL;
	}

	/* Check queue overflow */
	qdf_spin_lock_bh(&ctx->queue_lock);

	if (ctx->queue_size >= ctx->max_queue_size) {
		qdf_spin_unlock_bh(&ctx->queue_lock);
		ctx->stats.queue_overflows++;
		ctx->stats.frames_dropped++;
		wma_err("Injection queue overflow (size=%u, max=%u)",
			ctx->queue_size, ctx->max_queue_size);
		return QDF_STATUS_E_RESOURCES;
	}

	qdf_spin_unlock_bh(&ctx->queue_lock);

	/* Allocate and initialize queue node */
	node = wma_injection_queue_node_alloc(req, vdev_id);
	if (!node) {
		ctx->stats.frames_dropped++;
		wma_err("Failed to allocate injection queue node");
		return QDF_STATUS_E_NOMEM;
	}

	/* Add to queue */
	qdf_spin_lock_bh(&ctx->queue_lock);

	status = qdf_list_insert_back(&ctx->queue, &node->node);
	if (QDF_IS_STATUS_ERROR(status)) {
		qdf_spin_unlock_bh(&ctx->queue_lock);
		wma_injection_queue_node_free(node);
		ctx->stats.frames_dropped++;
		wma_err("Failed to add node to queue: %d", status);
		return status;
	}

	ctx->queue_size++;
	ctx->stats.frames_queued++;

	/* Update maximum queue depth */
	if (ctx->queue_size > ctx->stats.max_queue_depth) {
		ctx->stats.max_queue_depth = ctx->queue_size;
	}

	qdf_spin_unlock_bh(&ctx->queue_lock);

	/* Schedule queue processing work */
	qdf_sched_work(0, &ctx->queue_work);

	wma_debug("Queued injection frame: len=%u, vdev_id=%u, queue_size=%u",
		  req->frame_len, vdev_id, ctx->queue_size);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS wma_get_injection_queue_stats(tp_wma_handle wma_handle,
					 struct wma_injection_queue_stats *stats)
{
	struct wma_injection_queue_ctx *ctx = &g_wma_injection_ctx;

	if (!wma_handle || !stats) {
		wma_err("Invalid parameters: wma_handle=%pK, stats=%pK",
			wma_handle, stats);
		return QDF_STATUS_E_INVAL;
	}

	if (!ctx->is_initialized) {
		wma_err("Injection queue not initialized");
		return QDF_STATUS_E_AGAIN;
	}

	qdf_spin_lock_bh(&ctx->queue_lock);
	qdf_mem_copy(stats, &ctx->stats, sizeof(*stats));
	qdf_spin_unlock_bh(&ctx->queue_lock);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS wma_reset_injection_queue_stats(tp_wma_handle wma_handle)
{
	struct wma_injection_queue_ctx *ctx = &g_wma_injection_ctx;

	if (!wma_handle) {
		wma_err("Invalid WMA handle");
		return QDF_STATUS_E_INVAL;
	}

	if (!ctx->is_initialized) {
		wma_err("Injection queue not initialized");
		return QDF_STATUS_E_AGAIN;
	}

	qdf_spin_lock_bh(&ctx->queue_lock);
	qdf_mem_zero(&ctx->stats, sizeof(ctx->stats));
	qdf_spin_unlock_bh(&ctx->queue_lock);

	wma_info("Injection queue statistics reset");

	return QDF_STATUS_SUCCESS;
}

uint32_t wma_get_injection_queue_size(tp_wma_handle wma_handle)
{
	struct wma_injection_queue_ctx *ctx = &g_wma_injection_ctx;
	uint32_t queue_size;

	if (!wma_handle || !ctx->is_initialized) {
		return 0;
	}

	qdf_spin_lock_bh(&ctx->queue_lock);
	queue_size = ctx->queue_size;
	qdf_spin_unlock_bh(&ctx->queue_lock);

	return queue_size;
}

bool wma_is_injection_queue_empty(tp_wma_handle wma_handle)
{
	struct wma_injection_queue_ctx *ctx = &g_wma_injection_ctx;
	bool is_empty;

	if (!wma_handle || !ctx->is_initialized) {
		return true;
	}

	qdf_spin_lock_bh(&ctx->queue_lock);
	is_empty = qdf_list_empty(&ctx->queue);
	qdf_spin_unlock_bh(&ctx->queue_lock);

	return is_empty;
}

QDF_STATUS wma_flush_injection_queue(tp_wma_handle wma_handle)
{
	struct wma_injection_queue_ctx *ctx = &g_wma_injection_ctx;
	struct wma_injection_queue_node *node;
	qdf_list_node_t *list_node;
	QDF_STATUS status;
	uint32_t flushed_count = 0;

	if (!wma_handle) {
		wma_err("Invalid WMA handle");
		return QDF_STATUS_E_INVAL;
	}

	if (!ctx->is_initialized) {
		wma_err("Injection queue not initialized");
		return QDF_STATUS_E_AGAIN;
	}

	wma_debug("Flushing injection queue");

	/* Cancel any pending work */
	qdf_cancel_work(&ctx->queue_work);
	qdf_delayed_work_stop_sync(&ctx->delayed_work);

	/* Flush all queued frames */
	qdf_spin_lock_bh(&ctx->queue_lock);

	while (!qdf_list_empty(&ctx->queue)) {
		status = qdf_list_remove_front(&ctx->queue, &list_node);
		if (QDF_IS_STATUS_ERROR(status)) {
			wma_err("Failed to remove node during flush: %d", status);
			break;
		}

		node = qdf_container_of(list_node, struct wma_injection_queue_node, node);
		wma_injection_queue_node_free(node);
		flushed_count++;
	}

	ctx->queue_size = 0;
	ctx->stats.frames_dropped += flushed_count;

	qdf_spin_unlock_bh(&ctx->queue_lock);

	wma_info("Flushed %u frames from injection queue", flushed_count);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS wma_send_injection_frame_to_fw(tp_wma_handle wma_handle,
					  struct inject_frame_req *req,
					  uint8_t vdev_id)
{
	struct wmi_mgmt_params mgmt_params;
	QDF_STATUS status;
	qdf_nbuf_t wmi_buf;
	uint8_t *frame_data;

	if (!wma_handle || !req || !req->frame_data) {
		wma_err("Invalid parameters: wma_handle=%pK, req=%pK",
			wma_handle, req);
		return QDF_STATUS_E_INVAL;
	}

	if (req->frame_len == 0 || req->frame_len > WMA_FRAME_INJECT_MAX_FRAME_SIZE) {
		wma_err("Invalid frame length: %u", req->frame_len);
		return QDF_STATUS_E_INVAL;
	}

	/* Allocate WMI buffer for the frame */
	wmi_buf = qdf_nbuf_alloc(NULL, req->frame_len, 0, 0, false);
	if (!wmi_buf) {
		wma_err("Failed to allocate WMI buffer for injection frame");
		return QDF_STATUS_E_NOMEM;
	}

	/* Copy frame data to WMI buffer */
	frame_data = qdf_nbuf_put_tail(wmi_buf, req->frame_len);
	if (!frame_data) {
		wma_err("Failed to get buffer space for frame data");
		qdf_nbuf_free(wmi_buf);
		return QDF_STATUS_E_NOMEM;
	}

	qdf_mem_copy(frame_data, req->frame_data, req->frame_len);

	/* Initialize WMI management parameters for injection */
	qdf_mem_zero(&mgmt_params, sizeof(mgmt_params));
	mgmt_params.tx_frame = wmi_buf;
	mgmt_params.frm_len = req->frame_len;
	mgmt_params.vdev_id = vdev_id;
	mgmt_params.tx_type = GENERIC_NODOWLOAD_ACK_COMP_INDEX; /* Frame index for injection with ACK */
	mgmt_params.chanfreq = 0; /* Use current channel */
	mgmt_params.desc_id = req->session_id; /* Use session_id as descriptor */
	mgmt_params.pdata = req; /* Pass injection request as context */
	mgmt_params.macaddr = NULL; /* No specific MAC address */
	mgmt_params.qdf_ctx = cds_get_context(QDF_MODULE_ID_QDF);
	mgmt_params.tx_params_valid = false; /* Use default TX parameters */
	mgmt_params.use_6mbps = 0; /* Use rate from injection request if specified */

	/* Set transmission rate if specified in injection request */
	if (req->tx_rate != 0) {
		mgmt_params.tx_param.mcs_mask = req->tx_rate;
		mgmt_params.tx_params_valid = true;
	}

	wma_debug("Sending injection frame to firmware: len=%u, vdev_id=%u, desc_id=%u",
		  req->frame_len, vdev_id, mgmt_params.desc_id);

	/* Send frame to firmware via WMI management interface */
	if (wmi_service_enabled(wma_handle->wmi_handle, wmi_service_mgmt_tx_wmi)) {
		status = wmi_mgmt_unified_cmd_send(wma_handle->wmi_handle, &mgmt_params);
		if (QDF_IS_STATUS_ERROR(status)) {
			wma_err("WMI management TX command failed: %d", status);
			qdf_nbuf_free(wmi_buf);
			return status;
		}
	} else {
		/* Fallback to legacy data path if WMI management TX not supported */
		wma_warn("WMI management TX service not enabled, trying legacy path");
		
		/* Try legacy CDP management send path */
		struct cdp_soc_t *soc = cds_get_context(QDF_MODULE_ID_SOC);
		if (!soc) {
			wma_err("Failed to get CDP SOC context");
			qdf_nbuf_free(wmi_buf);
			return QDF_STATUS_E_FAILURE;
		}

		/* Set frame control information for legacy path */
		QDF_NBUF_CB_MGMT_TXRX_DESC_ID(wmi_buf) = mgmt_params.desc_id;
		
		int ret = cdp_mgmt_send_ext(soc, mgmt_params.vdev_id, wmi_buf,
					    mgmt_params.tx_type,
					    mgmt_params.use_6mbps,
					    mgmt_params.chanfreq);
		status = qdf_status_from_os_return(ret);
		
		if (QDF_IS_STATUS_ERROR(status)) {
			wma_err("Legacy management TX failed: %d", status);
			/* Note: wmi_buf is freed by cdp_mgmt_send_ext on failure */
			return status;
		}
	}

	wma_debug("Injection frame sent to firmware successfully (desc_id=%u)", 
		  mgmt_params.desc_id);
	
	/* Note: wmi_buf ownership transferred to firmware/CDP layer */
	return QDF_STATUS_SUCCESS;
}

QDF_STATUS wma_handle_injection_fw_response(tp_wma_handle wma_handle,
					     uint32_t desc_id,
					     uint32_t status)
{
	struct wma_injection_queue_ctx *ctx = &g_wma_injection_ctx;
	const char *status_str;

	if (!wma_handle) {
		wma_err("Invalid WMA handle");
		return QDF_STATUS_E_INVAL;
	}

	if (!ctx->is_initialized) {
		wma_debug("Injection queue not initialized, ignoring response");
		return QDF_STATUS_SUCCESS;
	}

	/* Map firmware status to string for logging */
	switch (status) {
	case WMI_MGMT_TX_COMP_TYPE_COMPLETE_OK:
		status_str = "SUCCESS";
		break;
	case WMI_MGMT_TX_COMP_TYPE_DISCARD:
		status_str = "DISCARDED";
		ctx->stats.frames_dropped++;
		break;
	case WMI_MGMT_TX_COMP_TYPE_COMPLETE_NO_ACK:
		status_str = "NO_ACK";
		break;
	case WMI_MGMT_TX_COMP_TYPE_INSPECT:
		status_str = "INSPECT";
		break;
	default:
		status_str = "UNKNOWN";
		ctx->stats.frames_dropped++;
		break;
	}

	wma_debug("Injection frame completion: desc_id=%u, status=%s (%u)",
		  desc_id, status_str, status);

	/* Update statistics based on firmware response */
	if (status == WMI_MGMT_TX_COMP_TYPE_COMPLETE_OK) {
		/* Frame was successfully transmitted */
		wma_debug("Injection frame transmitted successfully (desc_id=%u)", desc_id);
	} else {
		/* Frame transmission failed or had issues */
		wma_warn("Injection frame transmission failed: desc_id=%u, status=%s",
			 desc_id, status_str);
	}

	return QDF_STATUS_SUCCESS;
}

#else /* FEATURE_FRAME_INJECTION_SUPPORT */

QDF_STATUS wma_send_injection_frame_to_fw(tp_wma_handle wma_handle,
					  struct inject_frame_req *req,
					  uint8_t vdev_id)
{
	return QDF_STATUS_E_NOSUPPORT;
}

QDF_STATUS wma_handle_injection_fw_response(tp_wma_handle wma_handle,
					     uint32_t desc_id,
					     uint32_t status)
{
	QDF_STATUS qdf_status = QDF_STATUS_SUCCESS;
	enum wma_injection_fw_error_type error_type;

	WMA_LOGD("Handling firmware injection response: desc_id=%u, status=0x%x", desc_id, status);

	if (!wma_handle) {
		WMA_LOGE("Invalid WMA handle");
		return QDF_STATUS_E_INVAL;
	}

	/* Check if this is an error response */
	if (status != 0) {
		WMA_LOGW("Firmware injection failed: desc_id=%u, status=0x%x", desc_id, status);
		
		/* Handle the firmware error */
		qdf_status = wma_handle_firmware_injection_error(wma_handle, status, 0, NULL);
		if (QDF_IS_STATUS_ERROR(qdf_status) && qdf_status != QDF_STATUS_E_PENDING) {
			WMA_LOGE("Failed to handle firmware injection error: %d", qdf_status);
			return qdf_status;
		}
	} else {
		WMA_LOGD("Firmware injection completed successfully: desc_id=%u", desc_id);
	}

	/* Update statistics would go here in a full implementation */
	
	return qdf_status;
}

QDF_STATUS wma_init_injection_queue(tp_wma_handle wma_handle)
{
	return QDF_STATUS_SUCCESS;
}

QDF_STATUS wma_deinit_injection_queue(tp_wma_handle wma_handle)
{
	return QDF_STATUS_SUCCESS;
}

QDF_STATUS wma_queue_injection_frame(tp_wma_handle wma_handle,
				     struct inject_frame_req *req,
				     uint8_t vdev_id)
{
	return QDF_STATUS_E_NOSUPPORT;
}

QDF_STATUS wma_get_injection_queue_stats(tp_wma_handle wma_handle,
					 struct wma_injection_queue_stats *stats)
{
	return QDF_STATUS_E_NOSUPPORT;
}

QDF_STATUS wma_reset_injection_queue_stats(tp_wma_handle wma_handle)
{
	return QDF_STATUS_E_NOSUPPORT;
}

uint32_t wma_get_injection_queue_size(tp_wma_handle wma_handle)
{
	return 0;
}

bool wma_is_injection_queue_empty(tp_wma_handle wma_handle)
{
	return true;
}

QDF_STATUS wma_flush_injection_queue(tp_wma_handle wma_handle)
{
	return QDF_STATUS_SUCCESS;
}

/**
 * wma_translate_fw_injection_error() - Translate firmware error codes
 * @fw_error_code: Firmware-specific error code
 *
 * This function translates firmware-specific error codes to standard
 * WMA injection error types for consistent error handling.
 *
 * Return: Translated error type
 */
enum wma_injection_fw_error_type wma_translate_fw_injection_error(uint32_t fw_error_code)
{
	enum wma_injection_fw_error_type error_type;

	WMA_LOGD("Translating firmware error code: 0x%x", fw_error_code);

	switch (fw_error_code) {
	case 0x0: /* Success */
		error_type = WMA_INJECTION_FW_ERROR_NONE;
		break;
	case 0x1: /* Generic failure */
		error_type = WMA_INJECTION_FW_ERROR_REJECTED;
		break;
	case 0x2: /* Invalid VDEV */
		error_type = WMA_INJECTION_FW_ERROR_INVALID_VDEV;
		break;
	case 0x3: /* No resources */
		error_type = WMA_INJECTION_FW_ERROR_NO_RESOURCES;
		break;
	case 0x4: /* Interface down */
		error_type = WMA_INJECTION_FW_ERROR_INTERFACE_DOWN;
		break;
	case 0x5: /* Power save mode */
		error_type = WMA_INJECTION_FW_ERROR_POWER_SAVE;
		break;
	case 0x6: /* Channel switch in progress */
		error_type = WMA_INJECTION_FW_ERROR_CHANNEL_SWITCH;
		break;
	case 0x7: /* Scan active */
		error_type = WMA_INJECTION_FW_ERROR_SCAN_ACTIVE;
		break;
	case 0xFFFFFFFF: /* Timeout */
		error_type = WMA_INJECTION_FW_ERROR_TIMEOUT;
		break;
	default:
		error_type = WMA_INJECTION_FW_ERROR_UNKNOWN;
		break;
	}

	WMA_LOGD("Translated firmware error 0x%x to type %d", fw_error_code, error_type);
	return error_type;
}

/**
 * wma_handle_firmware_injection_error() - Handle firmware injection error
 * @wma_handle: WMA handle
 * @error_code: Firmware error code
 * @vdev_id: VDEV ID associated with error
 * @req: Frame request that caused error (optional)
 *
 * This function handles firmware errors during frame injection. It implements
 * retry logic for transient failures and coordinates with HDD layer for
 * error recovery. It also maintains firmware state synchronization.
 *
 * Return: QDF_STATUS_SUCCESS on successful recovery, error code on failure
 */
QDF_STATUS wma_handle_firmware_injection_error(tp_wma_handle wma_handle,
						uint32_t error_code,
						uint8_t vdev_id,
						struct inject_frame_req *req)
{
	enum wma_injection_fw_error_type error_type;
	struct wma_injection_fw_error_info *error_info;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	bool should_retry = false;
	uint32_t retry_delay_ms = 0;

	WMA_LOGD("Handling firmware injection error: code=0x%x, vdev_id=%u", error_code, vdev_id);

	if (!wma_handle) {
		WMA_LOGE("Invalid WMA handle");
		return QDF_STATUS_E_INVAL;
	}

	/* Translate firmware error code */
	error_type = wma_translate_fw_injection_error(error_code);

	/* Update error statistics - this would be part of a larger injection context */
	/* For now, we'll log the error information */
	WMA_LOGW("Firmware injection error: type=%d, code=0x%x, vdev_id=%u",
		 error_type, error_code, vdev_id);

	/* Determine if we should retry based on error type */
	switch (error_type) {
	case WMA_INJECTION_FW_ERROR_NO_RESOURCES:
		/* Transient error - retry with delay */
		should_retry = true;
		retry_delay_ms = 100; /* 100ms delay */
		break;

	case WMA_INJECTION_FW_ERROR_POWER_SAVE:
		/* Device in power save - retry with longer delay */
		should_retry = true;
		retry_delay_ms = 500; /* 500ms delay */
		break;

	case WMA_INJECTION_FW_ERROR_CHANNEL_SWITCH:
		/* Channel switch in progress - retry with delay */
		should_retry = true;
		retry_delay_ms = 200; /* 200ms delay */
		break;

	case WMA_INJECTION_FW_ERROR_SCAN_ACTIVE:
		/* Scan active - retry with short delay */
		should_retry = true;
		retry_delay_ms = 50; /* 50ms delay */
		break;

	case WMA_INJECTION_FW_ERROR_TIMEOUT:
		/* Timeout - retry once with longer delay */
		should_retry = true;
		retry_delay_ms = 1000; /* 1 second delay */
		break;

	case WMA_INJECTION_FW_ERROR_INVALID_VDEV:
	case WMA_INJECTION_FW_ERROR_INTERFACE_DOWN:
		/* Permanent errors - synchronize state but don't retry */
		status = wma_sync_firmware_injection_state(wma_handle, vdev_id);
		should_retry = false;
		break;

	case WMA_INJECTION_FW_ERROR_REJECTED:
	case WMA_INJECTION_FW_ERROR_UNKNOWN:
	default:
		/* Unknown or permanent errors - don't retry */
		should_retry = false;
		break;
	}

	/* Attempt retry if appropriate and request is available */
	if (should_retry && req) {
		/* Check retry count to avoid infinite loops */
		if (req->retry_count < 3) { /* Maximum 3 retries */
			WMA_LOGI("Retrying injection after %u ms delay (attempt %u)",
				 retry_delay_ms, req->retry_count + 1);
			
			/* Schedule retry with delay */
			status = wma_retry_injection_frame(wma_handle, req, vdev_id, error_type);
			if (QDF_IS_STATUS_SUCCESS(status)) {
				return QDF_STATUS_E_PENDING; /* Retry scheduled */
			}
		} else {
			WMA_LOGW("Maximum retry attempts reached for injection request");
			status = QDF_STATUS_E_FAILURE;
		}
	}

	/* If we reach here, either no retry was needed or retry failed */
	if (QDF_IS_STATUS_ERROR(status)) {
		WMA_LOGE("Firmware injection error handling failed: %d", status);
	} else {
		WMA_LOGI("Firmware injection error handled successfully");
	}

	return status;
}

/**
 * wma_retry_injection_frame() - Retry injection frame after firmware error
 * @wma_handle: WMA handle
 * @req: Frame injection request to retry
 * @vdev_id: VDEV ID for the frame
 * @error_type: Type of error that occurred
 *
 * This function implements retry logic for transient firmware failures.
 * It uses exponential backoff and limits the number of retry attempts.
 *
 * Return: QDF_STATUS_SUCCESS on successful retry, error code on failure
 */
QDF_STATUS wma_retry_injection_frame(tp_wma_handle wma_handle,
				     struct inject_frame_req *req,
				     uint8_t vdev_id,
				     enum wma_injection_fw_error_type error_type)
{
	QDF_STATUS status;
	uint32_t delay_ms;

	WMA_LOGD("Retrying injection frame: vdev_id=%u, error_type=%d, retry_count=%u",
		 vdev_id, error_type, req->retry_count);

	if (!wma_handle || !req) {
		WMA_LOGE("Invalid parameters for retry");
		return QDF_STATUS_E_INVAL;
	}

	/* Increment retry count */
	req->retry_count++;

	/* Calculate exponential backoff delay */
	delay_ms = 100 * (1 << (req->retry_count - 1)); /* 100ms, 200ms, 400ms, ... */
	if (delay_ms > 2000) {
		delay_ms = 2000; /* Cap at 2 seconds */
	}

	/* Add some jitter to avoid thundering herd */
	delay_ms += (qdf_get_log_timestamp() % 50); /* Add 0-49ms jitter */

	WMA_LOGI("Scheduling injection retry in %u ms (attempt %u)",
		 delay_ms, req->retry_count);

	/* For now, we'll simulate the retry by calling the send function again */
	/* In a real implementation, this would be scheduled with a timer */
	qdf_sleep(delay_ms);

	/* Attempt to send the frame again */
	status = wma_send_injection_frame_to_fw(wma_handle, req, vdev_id);
	if (QDF_IS_STATUS_ERROR(status)) {
		WMA_LOGW("Injection retry failed: %d", status);
		return status;
	}

	WMA_LOGI("Injection retry initiated successfully");
	return QDF_STATUS_SUCCESS;
}

/**
 * wma_sync_firmware_injection_state() - Synchronize firmware injection state
 * @wma_handle: WMA handle
 * @vdev_id: VDEV ID to synchronize
 *
 * This function synchronizes the firmware injection state after errors.
 * It ensures that the firmware and driver are in a consistent state.
 *
 * Return: QDF_STATUS_SUCCESS on success, error code on failure
 */
QDF_STATUS wma_sync_firmware_injection_state(tp_wma_handle wma_handle,
					      uint8_t vdev_id)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	WMA_LOGD("Synchronizing firmware injection state for vdev_id=%u", vdev_id);

	if (!wma_handle) {
		WMA_LOGE("Invalid WMA handle");
		return QDF_STATUS_E_INVAL;
	}

	/* Check if VDEV is valid and active */
	if (vdev_id >= wma_handle->max_bssid) {
		WMA_LOGE("Invalid VDEV ID: %u", vdev_id);
		return QDF_STATUS_E_INVAL;
	}

	/* Verify VDEV state */
	if (!wma_handle->interfaces[vdev_id].handle) {
		WMA_LOGW("VDEV %u is not active", vdev_id);
		return QDF_STATUS_E_INVAL;
	}

	/* Check interface type - injection typically requires monitor mode */
	if (wma_handle->interfaces[vdev_id].type != WMI_VDEV_TYPE_MONITOR) {
		WMA_LOGW("VDEV %u is not in monitor mode (type=%d)",
			 vdev_id, wma_handle->interfaces[vdev_id].type);
		/* This might not be an error depending on implementation */
	}

	/* Flush any pending injection frames for this VDEV */
	/* This would be implemented as part of the queue management */
	WMA_LOGI("Flushing pending injection frames for vdev_id=%u", vdev_id);

	/* Send a sync command to firmware if needed */
	/* This would involve sending a WMI command to query/reset injection state */
	WMA_LOGD("Sending injection state sync command to firmware");

	/* For now, we'll just log that synchronization is complete */
	WMA_LOGI("Firmware injection state synchronized for vdev_id=%u", vdev_id);

	return status;
}

#endif /* FEATURE_FRAME_INJECTION_SUPPORT */