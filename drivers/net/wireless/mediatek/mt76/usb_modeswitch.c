// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal ZeroCD automatic switch (storage mode -> communication mode)
 * Implemented inside mt76 only: register a USB notifier, detect device
 * 0x0e8d:0x2870 and send standard SCSI eject sequence to trigger re-enumeration.
 *
 * Note: USB binding is per interface. We try to send the switch command
 * as early as possible, but cannot fully guarantee a race-free order
 * against usb-storage on all systems.
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/usb/storage.h>
#include <scsi/scsi.h>

#define VENDOR_MEDIATEK  0x0e8d
#define PRODUCT_ZEROCD   0x2870

/* Android config uses 1/0 */
static int mt76_auto_modeswitch = 1;
module_param(mt76_auto_modeswitch, int, 0644);
MODULE_PARM_DESC(mt76_auto_modeswitch, "Enable auto mode switch for 0e8d:2870 (1=enable, 0=disable)");

struct msw_job {
    struct work_struct work;
    struct usb_device *udev;
    u8 ep_in;
    u8 ep_out;
};

static int mt76_send_bot_cmd(struct usb_device *udev, u8 ep_out, u8 ep_in,
                             const u8 *cdb, u8 cdb_len)
{
    int ret;
    unsigned int pipe_out = usb_sndbulkpipe(udev, ep_out);
    unsigned int pipe_in  = usb_rcvbulkpipe(udev, ep_in);
    struct bulk_cb_wrap *cbw;
    struct bulk_cs_wrap *csw;
    void *buf;
    int actlen;

    buf = kzalloc(max_t(size_t, US_BULK_CB_WRAP_LEN, US_BULK_CS_WRAP_LEN), GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    /* Send CBW */
    cbw = buf;
    cbw->Signature = cpu_to_le32(US_BULK_CB_SIGN);
    cbw->Tag = 0x12345678;
    cbw->DataTransferLength = cpu_to_le32(0);
    cbw->Flags = 0;
    cbw->Lun = 0;
    cbw->Length = cdb_len;
    memcpy(cbw->CDB, cdb, cdb_len);

    ret = usb_bulk_msg(udev, pipe_out, cbw, US_BULK_CB_WRAP_LEN, &actlen, 1000);
    if (ret)
        goto out;

    /* Receiving CSW */
    csw = buf;
    ret = usb_bulk_msg(udev, pipe_in, csw, US_BULK_CS_WRAP_LEN, &actlen, 1000);
    if (ret)
        goto out;

    if (csw->Status != US_BULK_STAT_OK)
        ret = -EIO;

out:
    kfree(buf);
    return ret;
}

static void mt76_msw_worker(struct work_struct *w)
{
    struct msw_job *job = container_of(w, struct msw_job, work);
    struct usb_device *udev = job->udev;
    int ret;
    u8 cdb[16] = {0};

    if (!mt76_auto_modeswitch)
        goto out;

    /* 1) Allow Medium Removal */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = ALLOW_MEDIUM_REMOVAL; /* 0x1E */
    ret = mt76_send_bot_cmd(udev, job->ep_out, job->ep_in, cdb, 6);
    if (ret)
        goto out;

    /* 2) Eject (LOEJ=1, START=0) */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = START_STOP; /* 0x1B */
    cdb[4] = 0x02;       /* LOEJ */
    mt76_send_bot_cmd(udev, job->ep_out, job->ep_in, cdb, 6);

out:
    usb_put_dev(udev);
    kfree(job);
}

static int mt76_try_queue_modeswitch(struct usb_device *udev)
{
    struct usb_host_config *cfg;
    struct usb_interface *intf;
    struct usb_host_interface *alts;
    int i;

    if (!mt76_auto_modeswitch)
        return 0;

    if (le16_to_cpu(udev->descriptor.idVendor) != VENDOR_MEDIATEK ||
        le16_to_cpu(udev->descriptor.idProduct) != PRODUCT_ZEROCD)
        return 0;

    cfg = udev->actconfig;
    if (!cfg)
        return 0;

    /* Find mass storage interface and its bulk in/out endpoints */
    for (i = 0; i < cfg->desc.bNumInterfaces; i++) {
        struct msw_job *job;
        int ep;
        u8 ep_in = 0, ep_out = 0;

        intf = cfg->interface[i];
        if (!intf)
            continue;
        alts = intf->cur_altsetting;
        if (!alts)
            continue;
        if (alts->desc.bInterfaceClass != USB_CLASS_MASS_STORAGE)
            continue;
        /* Only handle Bulk-Only protocol */
        if (alts->desc.bInterfaceProtocol != USB_PR_BULK)
            continue;

        for (ep = 0; ep < alts->desc.bNumEndpoints; ep++) {
            struct usb_endpoint_descriptor *e = &alts->endpoint[ep].desc;
            if (usb_endpoint_is_bulk_in(e))
                ep_in = usb_endpoint_num(e);
            else if (usb_endpoint_is_bulk_out(e))
                ep_out = usb_endpoint_num(e);
        }
        if (!ep_in || !ep_out)
            continue;

        job = kzalloc(sizeof(*job), GFP_KERNEL);
        if (!job)
            return -ENOMEM;
        INIT_WORK(&job->work, mt76_msw_worker);
        job->udev = usb_get_dev(udev);
        job->ep_in = ep_in;
        job->ep_out = ep_out;
        schedule_work(&job->work);
        return 1;
    }
    return 0;
}

static int mt76_usb_notify(struct notifier_block *self, unsigned long action, void *data)
{
    switch (action) {
    case USB_DEVICE_ADD: {
        struct usb_device *udev = data;
        mt76_try_queue_modeswitch(udev);
        break;
    }
    /* Only handle device events in this notifier */
    default:
        break;
    }
    return NOTIFY_OK;
}

static struct notifier_block mt76_usb_nb = {
    .notifier_call = mt76_usb_notify,
};

static int __init mt76_msw_init(void)
{
    usb_register_notify(&mt76_usb_nb);
    return 0;
}

static void __exit mt76_msw_exit(void)
{
    usb_unregister_notify(&mt76_usb_nb);
}

module_init(mt76_msw_init);
module_exit(mt76_msw_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("mt76: Auto ZeroCD mode switch (0e8d:2870)");
MODULE_AUTHOR("Project Dora Kernel");
