/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2013 Bert Vermeulen <bert@biot.com>
 * Copyright (C) 2013 DreamSourceLab <dreamsourcelab@dreamsourcelab.com>
 * Copyright (C) 2015 Diego F. Asanza <f.asanza@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/stat.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <libusb.h>
#include "dslogic.h"
#include "protocol.h"
#include "transfer.h"
#include <errno.h>
#include <string.h>
#include "libsigrok.h"
#include "libsigrok-internal.h"
#include <math.h>
#include "devdefs.h"

#ifndef _WIN32
#undef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif

struct ds_trigger_pos {
    uint32_t real_pos;
    uint32_t ram_saddr;
    unsigned char first_block[504];
};

struct dev_context {
    const dslogic_profile *profile;
    /*
    * Since we can't keep track of an DSLogic device after upgrading
    * the firmware (it renumerates into a different device address
    * after the upgrade) this is like a global lock. No device will open
    * until a proper delay after the last device was upgraded.
    */
    int64_t fw_updated;
    /* Device/capture settings */
    uint64_t current_samplerate;
    uint64_t sample_limit;
    clk_source clock_source; // EXTERNAL = TRUE
    clk_edge clock_edge;   // RISING = 0, FALLING = 1
    voltage_range voltage_threshold;
    gboolean filter;
    uint32_t capture_ratio;
    dev_mode device_mode;
    /* device internals */
    int64_t sample_count;
    unsigned int num_transfers;
    int submitted_transfers;
    int empty_transfer_count;
    void *cb_data;
    struct libusb_transfer **transfers;
    dslogic_status status;
    /* wide of sample. TRUE = 16 channels (2 bytes),
     * FALSE = 8 channels */
    gboolean sample_wide;

    /*sigrok context*/
    struct sr_context* ctx;

    /* unknow functions */
    int trigger_stage;
    uint16_t trigger_mask[NUM_TRIGGER_STAGES];
    uint16_t trigger_value[NUM_TRIGGER_STAGES];
    uint16_t trigger_buffer[NUM_TRIGGER_STAGES];
    uint64_t timebase;
    uint8_t trigger_slope;
    uint8_t trigger_source;
};

static void finish_acquisition(const struct sr_dev_inst *sdi) {
    struct dev_context *devc = sdi->priv;
    struct sr_datafeed_packet packet;
    sr_err("finish acquisition: send SR_DF_END packet");
    /* Terminate session. */
    packet.type = SR_DF_END;
    sr_session_send(devc->cb_data, &packet);
    sr_err("finish acquisition: remove fds from polling");
    if (devc->num_transfers != 0) {
        devc->num_transfers = 0;
        g_free(devc->transfers);
    }
    usb_source_remove(sdi->session, devc->ctx);
}

static void free_transfer(struct libusb_transfer *transfer)
{
    struct sr_dev_inst *sdi = transfer->user_data;
    struct dev_context *devc = sdi->priv;
    unsigned int i;

    g_free(transfer->buffer);
    transfer->buffer = NULL;
    libusb_free_transfer(transfer);

    for (i = 0; i < devc->num_transfers; i++) {
        if (devc->transfers[i] == transfer) {
            devc->transfers[i] = NULL;
            break;
        }
    }

    devc->submitted_transfers--;
    if (devc->submitted_transfers == 0 && devc->status != DSLOGIC_TRIGGERED)
        finish_acquisition(sdi);
}

SR_PRIV void abort_acquisition(const struct sr_dev_inst* sdi) {
    g_assert(sdi);
    struct dev_context *devc = sdi->priv;
    int i, ret;
    struct sr_usb_dev_inst *usb;
    devc->sample_count = -1;
    sr_info("%s: Stopping", __func__);
    /* Stop GPIF acquisition */
    usb = ((struct sr_dev_inst *) devc->cb_data)->conn;
    if ((ret = command_stop_acquisition (usb->devhdl)) != SR_OK)
        sr_err("Stop DSLogic acquisition failed!");
    else
        sr_info("Stop DSLogic acquisition!");
    /* Cancel exist transfers */
    for (i = devc->num_transfers - 1; i >= 0; i--) {
        if (devc->transfers[i]){
            libusb_cancel_transfer(devc->transfers[i]);
        }
    }
}

SR_PRIV int fpga_setting(const struct sr_dev_inst *sdi) {
    struct dev_context *devc;
    struct sr_usb_dev_inst *usb;
    struct libusb_device_handle *hdl;
    struct DSLogic_setting setting;
    int ret;
    int transferred;
    int result;
    int i;

    devc = sdi->priv;
    usb = sdi->conn;
    hdl = usb->devhdl;

    setting.sync = 0xf5a5f5a5;
    setting.mode_header = 0x0001;
    setting.divider_header = 0x0102ffff;
    setting.count_header = 0x0302ffff;
    setting.trig_pos_header = 0x0502ffff;
    setting.trig_glb_header = 0x0701;
    setting.trig_adp_header = 0x0a02ffff;
    setting.trig_sda_header = 0x0c02ffff;
    setting.trig_mask0_header = 0x1010ffff;
    setting.trig_mask1_header = 0x1110ffff;
    //setting.trig_mask2_header = 0x1210ffff;
    //setting.trig_mask3_header = 0x1310ffff;
    setting.trig_value0_header = 0x1410ffff;
    setting.trig_value1_header = 0x1510ffff;
    //setting.trig_value2_header = 0x1610ffff;
    //setting.trig_value3_header = 0x1710ffff;
    setting.trig_edge0_header = 0x1810ffff;
    setting.trig_edge1_header = 0x1910ffff;
    //setting.trig_edge2_header = 0x1a10ffff;
    //setting.trig_edge3_header = 0x1b10ffff;
    setting.trig_count0_header = 0x1c10ffff;
    setting.trig_count1_header = 0x1d10ffff;
    //setting.trig_count2_header = 0x1e10ffff;
    //setting.trig_count3_header = 0x1f10ffff;
    setting.trig_logic0_header = 0x2010ffff;
    setting.trig_logic1_header = 0x2110ffff;
    //setting.trig_logic2_header = 0x2210ffff;
    //setting.trig_logic3_header = 0x2310ffff;
    setting.end_sync = 0xfa5afa5a;

    //setting.mode = (test_mode ? 0x8000 : 0x0000) + trigger->trigger_en + (sdi->mode << 4);
    setting.mode = 1<<15; //((devc->op_mode == SR_OP_INTERNAL_TEST) << 15);
    setting.mode += 0<<14; //((devc->op_mode == SR_OP_EXTERNAL_TEST) << 14);
    setting.mode += 0<<13; //((devc->op_mode == SR_OP_LOOPBACK_TEST) << 13);
    setting.mode += 0; //trigger->trigger_en;
    setting.mode += 0<<4; //((sdi->mode > 0) << 4); 0=logic, 1= dso; 2 = analog
    setting.mode += ((devc->clock_source == CLOCK_EXT_CLK) << 1);
    setting.mode += (devc->clock_edge << 1);
    setting.mode += (((devc->current_samplerate == SR_MHZ(200) && 1/*sdi->mode != DSO*/) || (0/*sdi->mode == ANALOG*/)) << 5);
    setting.mode += ((devc->current_samplerate == SR_MHZ(400)) << 6);
    setting.mode += 0<<7; //((sdi->mode == ANALOG) << 7);
    setting.mode += 0<<8; //((devc->filter == SR_FILTER_1T) << 8); no filter
    setting.divider = (uint32_t) ceil(SR_MHZ(100) * 1.0 / devc->current_samplerate);
    setting.count = (uint32_t) (devc->sample_limit);
    setting.trig_pos = (uint32_t)(/*trigger->trigger_pos*/1 / 100.0f * devc->sample_limit); //danot sure about it.
    setting.trig_glb = 0; //trigger->trigger_stages; //no trigger stages
    setting.trig_adp = setting.count - setting.trig_pos - 1;
    setting.trig_sda = 0x0;
    if (1) {//trigger->trigger_mode == SIMPLE_TRIGGER) {
        setting.trig_mask0[0] = 0; //ds_trigger_get_mask0(TriggerStages);
        setting.trig_mask1[0] = 0; //ds_trigger_get_mask1(TriggerStages);

        setting.trig_value0[0] = 0; //ds_trigger_get_value0(TriggerStages);
        setting.trig_value1[0] = 0; //ds_trigger_get_value1(TriggerStages);

        setting.trig_edge0[0] = 0; //ds_trigger_get_edge0(TriggerStages);
        setting.trig_edge1[0] = 0; //ds_trigger_get_edge1(TriggerStages);

        setting.trig_count0[0] = 0; //trigger->trigger0_count[TriggerStages];
        setting.trig_count1[0] = 0; // trigger->trigger1_count[TriggerStages];

        setting.trig_logic0[0] = 0; //(trigger->trigger_logic[TriggerStages] << 1) + trigger->trigger0_inv[TriggerStages];
        setting.trig_logic1[0] = 0; //(trigger->trigger_logic[TriggerStages] << 1) + trigger->trigger1_inv[TriggerStages];

        for (i = 1; i < NUM_TRIGGER_STAGES; i++) {
            setting.trig_mask0[i] = 0xff;
            setting.trig_mask1[i] = 0xff;

            setting.trig_value0[i] = 0;
            setting.trig_value1[i] = 0;

            setting.trig_edge0[i] = 0;
            setting.trig_edge1[i] = 0;

            setting.trig_count0[i] = 0;
            setting.trig_count1[i] = 0;

            setting.trig_logic0[i] = 2;
            setting.trig_logic1[i] = 2;
        }
    } else {/*
        for (i = 0; i < NUM_TRIGGER_STAGES; i++) {
            setting.trig_mask0[i] = ds_trigger_get_mask0(i);
            setting.trig_mask1[i] = ds_trigger_get_mask1(i);

            setting.trig_value0[i] = ds_trigger_get_value0(i);
            setting.trig_value1[i] = ds_trigger_get_value1(i);

            setting.trig_edge0[i] = ds_trigger_get_edge0(i);
            setting.trig_edge1[i] = ds_trigger_get_edge1(i);

            setting.trig_count0[i] = trigger->trigger0_count[i];
            setting.trig_count1[i] = trigger->trigger1_count[i];

            setting.trig_logic0[i] = (trigger->trigger_logic[i] << 1) + trigger->trigger0_inv[i];
            setting.trig_logic1[i] = (trigger->trigger_logic[i] << 1) + trigger->trigger1_inv[i];
        }*/
    }

    result = SR_OK;
    ret = libusb_bulk_transfer(hdl, 2 | LIBUSB_ENDPOINT_OUT,
                               (unsigned char*)&setting, sizeof (struct DSLogic_setting),
                               &transferred, 1000);

    if (ret < 0) {
        sr_err("Unable to setting FPGA of DSLogic: %s.",
               libusb_error_name(ret));
        result = SR_ERR;
    } else if (transferred != sizeof (struct DSLogic_setting)) {
        sr_err("Setting FPGA error: expacted transfer size %d; actually %d",
               sizeof (struct DSLogic_setting), transferred);
        result = SR_ERR;
    }

    if (result == SR_OK)
        sr_info("FPGA setting done");

    return result;
}

SR_PRIV int fpga_config(struct libusb_device_handle *hdl, const char *filename) {
    FILE *fw;
    int offset, chunksize, ret, result;
    unsigned char *buf;
    int transferred;
    uint64_t filesize;
    struct stat f_stat;

    sr_info("Configure FPGA using %s", filename);
    if ((fw = g_fopen(filename, "rb")) == NULL) {
        sr_err("Unable to open FPGA bit file %s for reading: %s",
               filename, strerror(errno));
        return SR_ERR;
    }
    if (stat(filename, &f_stat) == -1)
        return SR_ERR;

    filesize = (uint64_t) f_stat.st_size;
    if (!(buf = g_try_malloc(filesize))) {
        sr_err("FPGA configure bit malloc failed.");
        return SR_ERR;
    }

    result = SR_OK;
    offset = 0;
    while (1) {
        chunksize = fread(buf, 1, filesize, fw);
        if (chunksize == 0)
            break;

        ret = libusb_bulk_transfer(hdl, 2 | LIBUSB_ENDPOINT_OUT,
                                   buf, chunksize,
                                   &transferred, 1000);

        if (ret < 0) {
            sr_err("Unable to configure FPGA of DSLogic: %s.",
                   libusb_error_name(ret));
            result = SR_ERR;
            break;
        } else if (transferred != chunksize) {
            sr_err("Configure FPGA error: expacted transfer size %d; actually %d",
                   chunksize, transferred);
            result = SR_ERR;
            break;
        }
        sr_info("Configure %d bytes", chunksize);
        offset += chunksize;
    }
    fclose(fw);
    if (result == SR_OK)
        sr_info("FPGA configure done");

    return result;
}

/**
 * Check the USB configuration to determine if this is an DSLogic device.
 *
 * @return TRUE if the device's configuration profile match DSLogic
 *         configuration, FALSE otherwise.
 */
SR_PRIV gboolean check_conf_profile(libusb_device *dev) {
    struct libusb_device_descriptor des;
    struct libusb_device_handle *hdl;
    gboolean ret;
    unsigned char strdesc[64];

    hdl = NULL;
    ret = FALSE;
    while (!ret) {
        /* Assume the FW has not been loaded, unless proven wrong. */
        if (libusb_get_device_descriptor(dev, &des) != 0)
            break;

        if (libusb_open(dev, &hdl) != 0)
            break;

        if (libusb_get_string_descriptor_ascii(hdl,
                                               des.iManufacturer, strdesc, sizeof (strdesc)) < 0)
            break;
        if (strncmp((const char *) strdesc, "DreamSourceLab", 14))
            break;

        if (libusb_get_string_descriptor_ascii(hdl,
                                               des.iProduct, strdesc, sizeof (strdesc)) < 0)
            break;
        if (strncmp((const char *) strdesc, "DSLogic", 7))
            break;

        /* If we made it here, it must be an DSLogic. */
        ret = TRUE;
    }
    if (hdl)
        libusb_close(hdl);

    return ret;
}

SR_PRIV int dev_status_get(struct sr_dev_inst *sdi, struct DSLogic_status *status) {
    printf("get_status\n");
    if (sdi) {
        struct sr_usb_dev_inst *usb;
        int ret;

        usb = sdi->conn;
        ret = command_get_status(usb->devhdl, status);
        if (ret != SR_OK) {
            sr_err("Device don't exist!");
            return SR_ERR;
        } else {
            return SR_OK;
        }
    } else {
        return SR_ERR;
    }
}

SR_PRIV int dev_test(struct sr_dev_inst *sdi) {
    printf("set_test\n");
    if (sdi) {
        struct sr_usb_dev_inst *usb;
        struct version_info vi;
        int ret;

        usb = sdi->conn;
        ret = command_get_fw_version(usb->devhdl, &vi);
        if (ret != SR_OK) {
            sr_err("Device don't exist!");
            return SR_ERR;
        } else {
            return SR_OK;
        }
    } else {
        return SR_ERR;
    }
}

SR_PRIV void receive_trigger_pos(struct libusb_transfer *transfer) {
    struct sr_dev_inst *sdi = transfer->user_data;
    struct dev_context *devc = sdi->priv;
    struct sr_datafeed_packet packet;
    struct sr_datafeed_logic logic;
    struct ds_trigger_pos *trigger_pos;
    int ret;
    sr_info("receive trigger pos handle...");
    if (devc->sample_count == -1) {
        free_transfer(transfer);
        return;
    }
    sr_info("receive_trigger_pos(): status %d; timeout %d; received %d bytes.",
            transfer->status, transfer->timeout, transfer->actual_length);
    if (devc->status != DSLOGIC_ERROR) {
        trigger_pos = (struct ds_trigger_pos *) transfer->buffer;
        switch (transfer->status) {
        case LIBUSB_TRANSFER_COMPLETED:
            packet.type = SR_DF_TRIGGER;
            packet.payload = trigger_pos;
            sr_session_send(devc->cb_data, &packet);
            packet.type = SR_DF_LOGIC;
            packet.payload = &logic;
            logic.unitsize = devc->sample_wide ? 2 : 1;
            logic.length = sizeof (trigger_pos->first_block);
            logic.data = trigger_pos->first_block;
            devc->sample_count += logic.length / logic.unitsize;
            sr_session_send(devc->cb_data, &packet);
            devc->status = DSLOGIC_TRIGGERED;
            free_transfer(transfer);
            devc->num_transfers = 0;
            break;
        default:
            //abort_acquisition(devc);
            free_transfer(transfer);
            devc->status = DSLOGIC_ERROR;
            break;
        }
        if (devc->status == DSLOGIC_TRIGGERED) {
            if ((ret = dev_transfer_start(devc->cb_data)) != SR_OK) {
                sr_err("%s: could not start data transfer"
                       "(%d)%d", __func__, ret, errno);
            }
        }
    }
}

SR_PRIV unsigned int get_timeout(struct dev_context *devc) {
    (void)devc;
    //size_t total_size;
    //unsigned int timeout;

    //total_size = get_buffer_size(devc) * get_number_of_transfers(devc);
    //timeout = total_size / to_bytes_per_ms(devc);
    //return timeout + timeout / 4; /* Leave a headroom of 25% percent. */
    return 1000;
}

SR_PRIV int dev_transfer_start(const struct sr_dev_inst *sdi) {
    struct dev_context *devc;
    struct sr_usb_dev_inst *usb;
    struct libusb_transfer *transfer;
    unsigned int i;
    unsigned int timeout;
    unsigned int num_transfers;
    int ret;
    unsigned char *buf;
    size_t size;

    devc = sdi->priv;
    usb = sdi->conn;

    timeout = get_timeout(devc);
    num_transfers = get_number_of_transfers(devc);
    size = get_buffer_size(devc);
    devc->submitted_transfers = 0;

    devc->transfers = g_try_malloc0(sizeof (*devc->transfers) * num_transfers);
    if (!devc->transfers) {
        sr_err("USB transfers malloc failed.");
        return SR_ERR_MALLOC;
    }

    devc->num_transfers = num_transfers;
    for (i = 0; i < num_transfers; i++) {
        if (!(buf = g_try_malloc(size))) {
            sr_err("USB transfer buffer malloc failed.");
            return SR_ERR_MALLOC;
        }
        transfer = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(transfer, usb->devhdl,
                                  6 | LIBUSB_ENDPOINT_IN, buf, size,
                                  dslogic_receive_transfer, sdi, 0);
        if ((ret = libusb_submit_transfer(transfer)) != 0) {
            sr_err("Failed to submit transfer: %s.",
                   libusb_error_name(ret));
            libusb_free_transfer(transfer);
            g_free(buf);
            abort_acquisition(sdi);
            return SR_ERR;
        }
        devc->transfers[i] = transfer;
        devc->submitted_transfers++;
    }

    devc->status = DSLOGIC_DATA;

    return SR_OK;
}

SR_PRIV unsigned int get_number_of_transfers(struct dev_context *devc) {
    unsigned int n;
    /* Total buffer size should be able to hold about 100ms of data. */
    n = 100 * to_bytes_per_ms(devc) / get_buffer_size(devc);

    if (n > NUM_SIMUL_TRANSFERS)
        return NUM_SIMUL_TRANSFERS;

    return n;
}

SR_PRIV unsigned int to_bytes_per_ms(struct dev_context *devc) {
    if (devc->current_samplerate > SR_MHZ(100))
        return SR_MHZ(100) / 1000 * (devc->sample_wide ? 2 : 1);
    else
        return devc->current_samplerate / 1000 * (devc->sample_wide ? 2 : 1);
}

SR_PRIV size_t get_buffer_size(struct dev_context *devc) {
    size_t s;

    /*
     * The buffer should be large enough to hold 20ms of data and
     * a multiple of 512.
     */
    s = 20 * to_bytes_per_ms(devc);
    return (s + 511) & ~511;
}

static int DSLogic_dev_open(struct sr_dev_inst *sdi, struct sr_dev_driver* di) {
    libusb_device **devlist;
    struct sr_usb_dev_inst *usb;
    struct libusb_device_descriptor des;
    struct dev_context *devc;
    struct drv_context *drvc;
    struct version_info vi;
    //int skip;
    int ret, i, device_count;
    uint8_t revid;
    char connection_id[64];

    drvc = di->priv;
    devc = sdi->priv;
    usb = sdi->conn;

    if (sdi->status == SR_ST_ACTIVE)
        /* Device is already in use. */
        return SR_ERR;
    device_count = libusb_get_device_list(drvc->sr_ctx->libusb_ctx, &devlist);
    if (device_count < 0) {
        sr_err("Failed to get device list: %s.",
               libusb_error_name(device_count));
        return SR_ERR;
    }

    for (i = 0; i < device_count; i++) {
        if ((ret = libusb_get_device_descriptor(devlist[i], &des))) {
            sr_err("Failed to get device descriptor: %s.",
                   libusb_error_name(ret));
            continue;
        }

        if (!dslogic_identify_by_vid_and_pid(devc, des.idVendor, des.idProduct))
            continue;

        if (sdi->status == SR_ST_INITIALIZING || sdi->status == SR_ST_INACTIVE) {
            /* check device by its physical usb bus/port address */
            usb_get_port_path(devlist[i], connection_id, sizeof (connection_id));
            if (strcmp(sdi->connection_id, connection_id))
                /* This is not the one. */
                continue;
        }

        if (!(ret = libusb_open(devlist[i], &usb->devhdl))) {
            if (usb->address == 0xff)
                /*
                 * First time we touch this device after FW
                 * upload, so we don't know the address yet.
                 */
                usb->address = libusb_get_device_address(devlist[i]);
        } else {
            sr_err("pFailed to open device: %s.",
                   libusb_error_name(ret));
            break;
        }

        ret = command_get_fw_version(usb->devhdl, &vi);
        if (ret != SR_OK) {
            sr_err("Failed to get firmware version.");
            break;
        }

        ret = command_get_revid_version(usb->devhdl, &revid);
        if (ret != SR_OK) {
            sr_err("Failed to get REVID.");
            break;
        }
        /*
         * Changes in major version mean incompatible/API changes, so
         * bail out if we encounter an incompatible version.
         * Different minor versions are OK, they should be compatible.
         */
        if (vi.major != DSLOGIC_REQUIRED_VERSION_MAJOR) {
            sr_err("Expected firmware version %d.x, "
                   "got %d.%d.", DSLOGIC_REQUIRED_VERSION_MAJOR,
                   vi.major, vi.minor);
            break;
        }

        sdi->status = SR_ST_ACTIVE;
        sr_info("Opened device %a on %d.%d, "
                "interface %d, firmware %d.%d.",
                sdi->connection_id, usb->bus, usb->address,
                USB_INTERFACE, vi.major, vi.minor);

        sr_info("Detected REVID=%d, it's a Cypress CY7C68013%s.",
                revid, (revid != 1) ? " (FX2)" : "A (FX2LP)");

        break;
    }

    libusb_free_device_list(devlist, 1);
    if (sdi->status != SR_ST_ACTIVE)
        return SR_ERR;
    return SR_OK;
}

// NEW API
//TODO: Remove
static const char* config_path = "/home/diego/media/DSLogic/dslogic-gui/res/";

SR_PRIV struct dev_context *dslogic_dev_new(void) {
    struct dev_context *devc;
    if (!(devc = g_try_malloc(sizeof (struct dev_context)))) {
        sr_err("Device context malloc failed.");
        return NULL;
    }
    devc->profile = NULL;
    devc->fw_updated = 0;
    devc->current_samplerate = DEFAULT_SAMPLERATE;
    devc->sample_limit = DEFAULT_SAMPLELIMIT;
    devc->sample_wide = 1; //TODO: autoadjust sample wide
    devc->clock_source = CLOCK_INTERNAL;
    devc->clock_edge = 0;
    devc->capture_ratio = 0;
    devc->voltage_threshold = VOLTAGE_RANGE_18_33_V;
    devc->filter = FALSE;
    devc->trigger_source = 0 ;
    devc->device_mode = NORMAL_MODE;
    return devc;
}


SR_PRIV gboolean dslogic_identify_by_vid_and_pid(struct dev_context* devc, int vid, int pid){
    return !(vid != devc->profile->vid
            || pid != devc->profile->pid);
}

SR_PRIV void dslogic_set_profile(struct dev_context* devc,const dslogic_profile* prof){
    devc->profile = prof;
}

SR_PRIV void dslogic_set_firmware_updated(struct dev_context* devc){
    devc->fw_updated = g_get_monotonic_time();
}

SR_PRIV int dslogic_dev_open(struct sr_dev_inst* sdi, struct sr_dev_driver* di){
    struct dev_context* devc = sdi->priv;
    uint64_t timediff_us, timediff_ms, ret;
    if (devc->fw_updated > 0) {
        sr_info("Waiting for device to reset.");
        /* Takes >= 300ms for the FX2 to be gone from the USB bus. */
        g_usleep(300 * 1000);
        timediff_ms = 0;
        while (timediff_ms < MAX_RENUM_DELAY_MS) {
            if ((ret = DSLogic_dev_open(sdi, di)) == SR_OK)
                break;
            g_usleep(100 * 1000);

            timediff_us = g_get_monotonic_time() - devc->fw_updated;
            timediff_ms = timediff_us / 1000;
            sr_spew("Waited %" PRIi64 "ms.", timediff_ms);
        }
        if (ret != SR_OK) {
            sr_err("Device failed to renumerate.");
            return SR_ERR;
        }
        sr_info("Device came back after %" PRIi64 "ms.", timediff_ms);
    } else {
        sr_info("Firmware upload was not needed.");
        ret = DSLogic_dev_open(sdi,di);
    }
    return ret;
}

SR_PRIV int dslogic_configure_fpga(struct sr_dev_inst* sdi){
    struct dev_context* devc = sdi->priv;
    struct sr_usb_dev_inst* usb = sdi->conn;
    int ret;
    if ((ret = command_fpga_config(usb->devhdl)) != SR_OK) {
        sr_err("Send FPGA configure command failed!");
    } else {
        /* Takes >= 10ms for the FX2 to be ready for FPGA configure. */
        g_usleep(10 * 1000);
        char filename[256];
        switch (devc->voltage_threshold) {
        case VOLTAGE_RANGE_18_33_V:
            sprintf(filename, "%s%s", config_path, devc->profile->fpga_bit33);
            break;
        case VOLTAGE_RANGE_5_V:
            sprintf(filename, "%s%s", config_path, devc->profile->fpga_bit50);
            break;
        default:
            sr_err("wrong voltage settings");
            return SR_ERR;
        }
        const char *fpga_bit = filename;
        ret = fpga_config(usb->devhdl, fpga_bit);
        if (ret != SR_OK) {
            sr_err("Configure FPGA failed!");
        }
    }
    return ret;
}

SR_PRIV uint64_t dslogic_get_sample_limit(const struct sr_dev_inst* sdi){
    struct dev_context* devc = sdi->priv;
    return devc->sample_limit;
}

SR_PRIV int dslogic_set_sample_limit(const struct sr_dev_inst* sdi, uint64_t value){
    g_assert(sdi);
    if(value > SR_MB(16))
        return SR_ERR_ARG;
    struct dev_context* devc = sdi->priv;
    devc->sample_limit = value;
    sr_dbg("New Sample Limit: %d", devc->sample_limit);
    return SR_OK;
}

SR_PRIV voltage_range dslogic_get_voltage_threshold(const struct sr_dev_inst* sdi){
    g_assert(sdi);
    struct dev_context* devc = sdi->priv;
    return devc->voltage_threshold;
}

SR_PRIV int dslogic_set_voltage_threshold(const struct sr_dev_inst* sdi, voltage_range value){
    g_assert(sdi);
    struct dev_context* devc = sdi->priv;
    int ret;
    devc->voltage_threshold = value;
    dslogic_configure_fpga(sdi);
    return ret;
}

SR_PRIV dev_mode dslogic_get_device_mode(const struct sr_dev_inst* sdi){
    g_assert(sdi);
    struct dev_context* devc = sdi->priv;
    return devc->device_mode;
}

SR_PRIV int dslogic_set_device_mode(const struct sr_dev_inst* sdi, dev_mode value){
    g_assert(sdi);
    struct dev_context* devc = sdi->priv;
    devc->device_mode = value;
    return SR_OK;
}

SR_PRIV uint64_t dslogic_get_sample_rate(const struct sr_dev_inst* sdi){
    g_assert(sdi);
    struct dev_context* devc = sdi->priv;
    return devc->current_samplerate;
}

SR_PRIV int dslogic_set_samplerate(const struct sr_dev_inst* sdi, uint64_t samplerate){
    g_assert(sdi);
    struct dev_context* devc = sdi->priv;
    if(samplerate > SR_MB(400))
        return SR_ERR_ARG;
    devc->current_samplerate = samplerate;
    sr_dbg("New samplerate: %d", devc->current_samplerate);
    return SR_OK;
}

SR_PRIV void dslogic_acquisition_stop(const struct sr_dev_inst* sdi){
    g_assert(sdi);
    struct dev_context *devc;
    devc = sdi->priv;
    devc->status = DSLOGIC_STOP;
    sr_info("%s: Stopping", __func__);
    abort_acquisition(sdi);
}

SR_PRIV uint64_t dslogic_get_capture_ratio(const struct sr_dev_inst* sdi){
    g_assert(sdi);
    struct dev_context *devc = sdi->priv;
    return devc->capture_ratio;
}

SR_PRIV int dslogic_set_capture_ratio(const struct sr_dev_inst* sdi, uint64_t ratio){
    g_assert(sdi);
    struct dev_context *devc = sdi->priv;
    if(ratio > 100)
        return SR_ERR_ARG;
    devc->capture_ratio = ratio;
    return SR_OK;
}

SR_PRIV int dslogic_get_sample_count(const struct sr_dev_inst* sdi){
    g_assert(sdi);
    struct dev_context *devc = sdi->priv;
    return devc->sample_count;
}

SR_PRIV dslogic_status dslogic_get_device_status(const struct sr_dev_inst* sdi){
    g_assert(sdi);
    struct dev_context *devc = sdi->priv;
    return devc->status;
}

SR_PRIV void dslogic_clear_trigger_stages(const struct sr_dev_inst* sdi){
    g_assert(sdi);
    struct dev_context *devc = sdi->priv;
    int i;
    for (i = 0; i < NUM_TRIGGER_STAGES; i++) {
        devc->trigger_mask[i] = 0;
        devc->trigger_value[i] = 0;
    }
}

SR_PRIV void dslogic_set_sample_wide(const struct sr_dev_inst* sdi, int wide){
    g_assert(sdi);
    struct dev_context *devc = sdi->priv;
    if ( wide > 7 )
        devc->sample_wide = TRUE;
    else
        devc->sample_wide = FALSE;
    sr_dbg("setting sample wide: %d, Num Probes: %d", devc->sample_wide, wide);
}

SR_PRIV void dslogic_set_trigger_stage(const struct sr_dev_inst* sdi){
    g_assert(sdi);
    struct dev_context *devc = sdi->priv;
    devc->trigger_stage = TRIGGER_FIRED;
}

SR_PRIV int dslogic_send_fpga_settings(const struct sr_dev_inst* sdi,
                                       void* cb_data){
    g_assert(sdi);
    struct dev_context* devc = sdi->priv;
    struct sr_usb_dev_inst* usb = sdi->conn;
    int ret = SR_OK;
    /* Stop Previous GPIF acquisition */
    devc->cb_data = cb_data;
    devc->sample_count = 0;
    devc->empty_transfer_count = 0;
    devc->status = DSLOGIC_INIT;
    devc->num_transfers = 0;
    devc->submitted_transfers = 0;
    if ((ret = command_stop_acquisition(usb->devhdl)) != SR_OK) {
        sr_err("Stop DSLogic acquisition failed!");
        abort_acquisition(sdi);
        return ret;
    } else {
        sr_info("Previous DSLogic acquisition stopped!");
    }
    /* Setting FPGA before acquisition start*/
    if ((ret = command_fpga_setting(usb->devhdl, sizeof (struct DSLogic_setting) / sizeof (uint16_t))) != SR_OK) {
        sr_err("Send FPGA setting command failed!");
    } else {
        if ((ret = fpga_setting(sdi)) != SR_OK) {
            sr_err("Configure FPGA failed!");
            abort_acquisition(sdi);
            return ret;
        }
    }
    return ret;
}

SR_PRIV int dslogic_set_usb_transfer(struct sr_dev_inst* sdi,
                                     struct sr_dev_driver * di,
                                     sr_receive_data_callback receive_data){
    g_assert(sdi);
    g_assert(di);
    struct dev_context* devc = sdi->priv;
    struct drv_context* drvc = di->priv;
    struct sr_usb_dev_inst* usb = sdi->conn;
    struct ds_trigger_pos* trigger_pos;
    struct libusb_transfer *transfer;
    int ret;
    if ((ret = command_start_acquisition(usb->devhdl,devc->current_samplerate,
                                         devc->sample_wide, TRUE)) != SR_OK) {
        abort_acquisition(sdi);
        return ret;
    }
    devc->transfers = g_try_malloc0(sizeof (*devc->transfers));
    if (!devc->transfers) {
        sr_err("USB trigger_pos transfer malloc failed.");
        return SR_ERR_MALLOC;
    }
    //poll trigger status transfer
    if (!(trigger_pos = g_try_malloc0(sizeof (struct ds_trigger_pos)))) {
        sr_err("USB trigger_pos buffer malloc failed.");
        return SR_ERR_MALLOC;
    }
    devc->num_transfers = 1;
    transfer = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(transfer, usb->devhdl,
                              6 | LIBUSB_ENDPOINT_IN,
                              (unsigned char*)trigger_pos,
                              sizeof (struct ds_trigger_pos),
                              receive_trigger_pos, sdi, 0);
    if ((ret = libusb_submit_transfer(transfer)) != 0) {
        sr_err("Failed to submit trigger_pos transfer: %s.",
               libusb_error_name(ret));
        libusb_free_transfer(transfer);
        g_free(trigger_pos);
        abort_acquisition(sdi);
        return SR_ERR;
    }
    devc->ctx = drvc->sr_ctx;
    usb_source_add(sdi->session, drvc->sr_ctx, get_timeout(devc), receive_data, (void*) sdi);
    devc->transfers[0] = transfer;
    devc->submitted_transfers++;
    devc->status = DSLOGIC_START;
    // Send header packet to the session bus.
    //std_session_send_df_header(cb_data, LOG_PREFIX);
    std_session_send_df_header(sdi, LOG_PREFIX);
}

SR_PRIV void dslogic_receive_transfer(struct libusb_transfer *transfer) {
    gboolean packet_has_error = FALSE;
    struct sr_datafeed_packet packet;
    struct sr_datafeed_logic logic;
    /*
     struct sr_datafeed_dso dso;
     */
    //struct sr_datafeed_analog analog;
    struct sr_dev_inst* sdi = transfer->user_data;
    struct dev_context *devc = sdi->priv;
    int trigger_offset;
    int i, sample_width, cur_sample_count;
    int trigger_offset_bytes;
    uint8_t *cur_buf;
    GTimeVal cur_time;
    g_get_current_time(&cur_time);

    sr_dbg("receive_transfer: current time %d sec %d usec",
           cur_time.tv_sec, cur_time.tv_usec);
    /*
     * If acquisition has already ended, just free any queued up
     * transfer that come in.
     */
    if (devc->sample_count == -1) {
        sr_dbg("receive_data: already ended");
        free_transfer(transfer);
        return;
    }

    sr_dbg("receive_transfer: status %d; timeout %d; received %d bytes.",
           transfer->status, transfer->timeout, transfer->actual_length);

    /* Save incoming transfer before reusing the transfer struct. */
    cur_buf = transfer->buffer;
    sample_width = devc->current_samplerate <= SR_MHZ(100) ? 2 :
                                                             devc->sample_wide ? 2 : 1;
    cur_sample_count = transfer->actual_length / sample_width;
    switch (transfer->status) {
        case LIBUSB_TRANSFER_NO_DEVICE:
            //abort_acquisition(devc);
            free_transfer(transfer);
            devc->status = DSLOGIC_ERROR;
            return;
        case LIBUSB_TRANSFER_COMPLETED:
        case LIBUSB_TRANSFER_TIMED_OUT: /* We may have received some data though. */
            break;
        default:
            packet_has_error = TRUE;
            break;
    }

    if (transfer->actual_length == 0 || packet_has_error) {
        devc->empty_transfer_count++;
        if (devc->empty_transfer_count > MAX_EMPTY_TRANSFERS) {
            /*
                 * The FX2 gave up. End the acquisition, the frontend
                 * will work out that the samplecount is short.
                 */
            //abort_acquisition(devc);
            sr_dbg("receive_transfer: error");
            free_transfer(transfer);
            devc->status = DSLOGIC_ERROR;
        } else {
            sr_dbg("receive_transfer: resubmit");
            resubmit_transfer(transfer);
        }
        return;
    } else {
        sr_dbg("receive_transfer: empty_transfer_count=0");
        devc->empty_transfer_count = 0;
    }

    trigger_offset = 0;
    if (devc->trigger_stage >= 0) {
        for (i = 0; i < cur_sample_count; i++) {
            const uint16_t cur_sample = devc->sample_wide ?
                        *((const uint16_t*) cur_buf + i) :
                        *((const uint8_t*) cur_buf + i);
            if ((cur_sample & devc->trigger_mask[devc->trigger_stage]) ==
                    devc->trigger_value[devc->trigger_stage]) {
                // Match on this trigger stage.
                devc->trigger_buffer[devc->trigger_stage] = cur_sample;
                devc->trigger_stage++;
                if (devc->trigger_stage == NUM_TRIGGER_STAGES ||
                        devc->trigger_mask[devc->trigger_stage] == 0) {
                    // Match on all trigger stages, we're done.
                    trigger_offset = i + 1;
                    /*
                     * TODO: Send pre-trigger buffer to session bus.
                     * Tell the frontend we hit the trigger here.
                     */
                    packet.type = SR_DF_TRIGGER;
                    packet.payload = NULL;
                    sr_session_send(devc->cb_data, &packet);
                    /*
                     * Send the samples that triggered it,
                     * since we're skipping past them.
                     */
                    packet.type = SR_DF_LOGIC;
                    packet.payload = &logic;
                    logic.unitsize = sizeof (*devc->trigger_buffer);
                    logic.length = devc->trigger_stage * logic.unitsize;
                    logic.data = devc->trigger_buffer;
                    sr_session_send(devc->cb_data, &packet);
                    devc->trigger_stage = TRIGGER_FIRED;
                    break;
                }
            } else if (devc->trigger_stage > 0) {
                /*
                 * We had a match before, but not in the next sample. However, we may
                 * have a match on this stage in the next bit -- trigger on 0001 will
                 * fail on seeing 00001, so we need to go back to stage 0 -- but at
                 * the next sample from the one that matched originally, which the
                 * counter increment at the end of the loop takes care of.
                    */
                i -= devc->trigger_stage;
                if (i < -1)
                    i = -1; // Oops, went back past this buffer.
                // Reset trigger stage.
                devc->trigger_stage = 0;
            }
        }
    }
    if (devc->trigger_stage == TRIGGER_FIRED) {
        /* Send the incoming transfer to the session bus. */
        trigger_offset_bytes = trigger_offset * sample_width;
        packet.type = SR_DF_LOGIC;
        packet.payload = &logic;
        logic.length = transfer->actual_length - trigger_offset_bytes;
        logic.unitsize = sample_width;
        logic.data = cur_buf + trigger_offset_bytes;
        if ((devc->sample_limit && devc->sample_count < (int64_t)devc->sample_limit)) {
            const uint64_t remain_length = (devc->sample_limit - devc->sample_count) * sample_width;
            logic.length = min(logic.length, remain_length);
            // send data to session bus
            sr_session_send(devc->cb_data, &packet);
        }

        devc->sample_count += cur_sample_count;
        if ( devc->sample_limit &&
                (unsigned int)devc->sample_count >= devc->sample_limit) {
            //abort_acquisition(devc);
            free_transfer(transfer);
            devc->status = DSLOGIC_STOP;
            return;
        }
    } else {
        // TODO: Buffer pre-trigger data in capture
        //ratio-sized buffer.
    }
    resubmit_transfer(transfer);
}
