/*
 * Elan SPI driver for libfprint
 *
 * Copyright (C) 2021 Matthew Mirvish <matthew@mm12.xyz>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "elanspi"

#include "drivers_api.h"
#include "elanspi.h"
#include "fpi-spi-transfer.h"
#include "fpi-log.h"

#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <errno.h>

struct _FpiDeviceElanSpi {
	FpImageDevice parent;

	/* sensor info */
	guint8 sensor_width, sensor_height, sensor_ic_version, sensor_id;
	gboolean sensor_otp;
	guint8 sensor_vcm_mode;
	/* end sensor info */

	/* init info */
	guint8 sensor_raw_version, sensor_reg_17;
	guint8 sensor_reg_vref1, sensor_reg_28, sensor_reg_27, sensor_reg_dac2;
	/* end init info */

	/* calibration info */
	union {
		struct {
			guint8 dac_value;
			guint8 otp_timeout;
			guint8 line_ptr;
		} old_data;
		struct {
			guint16 gdac_value;
		} hv_data;
	};

	/* generic temp info for async reading */
	guint8 sensor_status;

	/* background / calibration parameters */
	guint16 *bg_image;
	guint16 *last_image;

	/* active SPI status info */
	int spi_fd;
};

G_DECLARE_FINAL_TYPE(FpiDeviceElanSpi, fpi_device_elanspi, FPI, DEVICE_ELANSPI, FpImageDevice);
G_DEFINE_TYPE(FpiDeviceElanSpi, fpi_device_elanspi, FP_TYPE_IMAGE_DEVICE);

static void elanspi_do_hwreset(FpiDeviceElanSpi *self, GError **err) {
	/*
	 * TODO: Make this also work with the non-HID cases
	 */

	int fd = open((char *)fpi_device_get_udev_data(FP_DEVICE(self), FP_DEVICE_UDEV_SUBTYPE_HIDRAW), O_RDWR);
	if (fd < 0) {
		g_set_error(err, G_IO_ERROR, g_io_error_from_errno(errno), "unable to open hid");
		return;
	}

	guint8 buf[5] = {
		0xe, 0, 0, 0, 0
	};

	if (ioctl(fd, HIDIOCSFEATURE(5), &buf) != 5) {
		g_set_error(err, G_IO_ERROR, g_io_error_from_errno(errno), "unable to reset via hid");
		return;
	}

	close(fd);
}

/*
 * Three main processes involved in driving these sensors:
 *   - initialization (device type detection)
 *   - calibration
 *      - image capture (single)
 *   - image capture (stitched)
 */

enum elanspi_init_state {
	ELANSPI_INIT_READ_STATUS1,
	ELANSPI_INIT_HWSWRESET, // fused b.c. hw reset is currently sync
	ELANSPI_INIT_READ_HEIGHT,
	ELANSPI_INIT_READ_WIDTH,
	ELANSPI_INIT_READ_REG17,   // both of these states finish setting up sensor settings
	ELANSPI_INIT_READ_VERSION, // can jump straight to calibrate
	ELANSPI_INIT_SWRESET2,
	ELANSPI_INIT_OTP_READ_VREF1,
	ELANSPI_INIT_OTP_WRITE_VREF1,
	ELANSPI_INIT_OTP_WRITE_0x28,
	ELANSPI_INIT_OTP_LOOP_READ_0x28, // may loop
	ELANSPI_INIT_OTP_LOOP_READ_0x27,
	ELANSPI_INIT_OTP_LOOP_UPDATEDAC_READ_DAC2,
	ELANSPI_INIT_OTP_LOOP_UPDATEDAC_WRITE_DAC2,
	ELANSPI_INIT_OTP_LOOP_UPDATEDAC_WRITE_10,
	// exit loop
	ELANSPI_INIT_OTP_WRITE_0xb,
	ELANSPI_INIT_OTP_WRITE_0xc,
	// do calibration (mutexc)
	ELANSPI_INIT_CALIBRATE,
	ELANSPI_INIT_NSTATES
};

enum elanspi_calibrate_old_state {
	ELANSPI_CALIBOLD_UNPROTECT,
	ELANSPI_CALIBOLD_WRITE_STARTCALIB,
	ELANSPI_CALIBOLD_SEND_REGTABLE,
	// calibrate dac base value
	ELANSPI_CALIBOLD_DACBASE_CAPTURE,
	ELANSPI_CALIBOLD_DACBASE_WRITE_DAC1,
	// check for finger
	ELANSPI_CALIBOLD_CHECKFIN_CAPTURE,
	// increase gain
	ELANSPI_CALIBOLD_WRITE_GAIN,
	// calibrate dac stage2
	ELANSPI_CALIBOLD_DACFINE_CAPTURE,
	ELANSPI_CALIBOLD_DACFINE_WRITE_DAC1,
	// exit ok
	ELANSPI_CALIBOLD_PROTECT,
	// capture bg image
	ELANSPI_CALIBOLD_BG_CAPTURE, // jumps to end of ssm on ok
	// jump to reset to 0 but fail too
	ELANSPI_CALIBOLD_EXIT_ERR,
	ELANSPI_CALIBOLD_NSTATES
};

enum elanspi_capture_old_state {
	ELANSPI_CAPTOLD_WRITE_CAPTURE,
	ELANSPI_CAPTOLD_CHECK_LINEREADY,
	ELANSPI_CAPTOLD_RECV_LINE,

	ELANSPI_CAPTOLD_NSTATES
};

enum elanspi_calibrate_hv_state {
	ELANSPI_CALIBHV_SELECT_PAGE0_0,
	ELANSPI_CALIBHV_WRITE_STARTCALIB,
	ELANSPI_CALIBHV_UNPROTECT,
	ELANSPI_CALIBHV_SEND_REGTABLE0,
	ELANSPI_CALIBHV_SELECT_PAGE1,
	ELANSPI_CALIBHV_SEND_REGTABLE1,
	ELANSPI_CALIBHV_SELECT_PAGE0_1,
	ELANSPI_CALIBHV_WRITE_GDAC_H,
	ELANSPI_CALIBHV_WRITE_GDAC_L,
	ELANSPI_CALIBHV_CAPTURE,
	ELANSPI_CALIBHV_WRITE_BEST_GDAC_H,
	ELANSPI_CALIBHV_WRITE_BEST_GDAC_L,
	
	ELANSPI_CALIBHV_NSTATES
};

enum elanspi_capture_hv_state {
	ELANSPI_CAPTHV_WRITE_CAPTURE,
	ELANSPI_CHECK_READY,
	ELANSPI_RECV_IMAGE
};

enum elanspi_write_regtable_state {
	ELANSPI_WRTABLE_WRITE,
	ELANSPI_WRTABLE_ITERATE,
	ELANSPI_WRTABLE_NSTATES
};

/* helpers */

static void elanspi_do_hwreset(FpiDeviceElanSpi *self, GError **err);
static void elanspi_do_swreset(FpiDeviceElanSpi *self, FpiSpiTransfer *xfer);
static void elanspi_do_startcalib(FpiDeviceElanSpi *self, FpiSpiTransfer *xfer);
static void elanspi_do_selectpage(FpiDeviceElanSpi *self, guint8 page, FpiSpiTransfer *xfer);

static void elanspi_single_read_cmd(FpiDeviceElanSpi *self, guint8 cmd_id, guint8 *data_out, FpiSpiTransfer *xfer);

static void elanspi_read_status(FpiDeviceElanSpi *self, guint8 *data_out, FpiSpiTransfer *xfer);
static void elanspi_read_width(FpiDeviceElanSpi *self, guint8 *data_out, FpiSpiTransfer *xfer);
static void elanspi_read_height(FpiDeviceElanSpi *self, guint8 *data_out, FpiSpiTransfer *xfer);
static void elanspi_read_version(FpiDeviceElanSpi *self, guint8 *data_out, FpiSpiTransfer *xfer);

static void elanspi_read_register(FpiDeviceElanSpi *self, guint8 reg_id, guint8 *data_out, FpiSpiTransfer *xfer);
static void elanspi_write_register(FpiDeviceElanSpi *self, guint8 reg_id, guint8 data_in, FpiSpiTransfer *xfer);

static void elanspi_determine_sensor(FpiDeviceElanSpi *self, GError **err) {
	guint8 raw_height = self->sensor_height;
	guint8 raw_width = self->sensor_width;

	if ( ((raw_height == 0xa1) && (raw_width == 0xa1)) ||
	     ((raw_height == 0xd1) && (raw_width == 0x51)) ||
	     ((raw_height == 0xc1) && (raw_width == 0x39)) ) {
		self->sensor_ic_version = 0; // Version 0
		self->sensor_width = raw_width - 1;
		self->sensor_height = raw_height - 1;
	}
	else {
		// If the sensor is exactly 96x96 (0x60 x 0x60), the version is the high bit of register 17
		if (raw_width == 0x60 && raw_height == 0x60) {
			self->sensor_ic_version = (self->sensor_reg_17 & 0x80) ? 1 : 0;
		}
		else {
			if ( ((raw_height != 0xa0) || (raw_width != 0x50)) &&
				 ((raw_height != 0x90) || (raw_width != 0x40)) &&
				 ((raw_height != 0x78) || (raw_width != 0x78)) ) {
				if ( ((raw_height != 0x40) || (raw_width != 0x58)) &&
				     ((raw_height != 0x50) || (raw_width != 0x50)) ) {
					// Old sensor hack??
					self->sensor_width = 0x78;
					self->sensor_height = 0x78;
					self->sensor_ic_version = 0;
				}
				else {
					// Otherwise, read the version 'normally'
					self->sensor_ic_version = (self->sensor_raw_version & 0x70) >> 4;
				}
			}
			else {
				self->sensor_ic_version = 1;
			}
		}
	}

	fp_dbg("<init/detect> after hardcoded lookup; %dx%d, version %d", self->sensor_width, self->sensor_height, self->sensor_ic_version);

	for (const struct elanspi_sensor_entry *entry = elanspi_sensor_table; entry->name; ++entry) {
		if (entry->ic_version == self->sensor_ic_version && entry->width == self->sensor_width && entry->height == self->sensor_height) {
			self->sensor_id = entry->sensor_id;
			self->sensor_otp = entry->is_otp_model;

			fp_dbg("<init/detect> found sensor ID %d => [%s] (%d x %d)", self->sensor_id, entry->name, self->sensor_width, self->sensor_height);
			break;
		}
	}

	if (self->sensor_id == 0xff) {
		*err = fpi_device_error_new_msg(FP_DEVICE_ERROR_NOT_SUPPORTED, "unknown sensor (%dx%d, v%d)", self->sensor_width, self->sensor_height, self->sensor_ic_version);
		return;
	}
}

static void elanspi_init_ssm_handler(FpiSsm *ssm, FpDevice *dev) {
	FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI(dev);
	FpiSpiTransfer   *xfer = NULL;
	GError           *err  = NULL;
	FpiSsm           *chld = NULL;

	switch (fpi_ssm_get_cur_state(ssm)) {
		case ELANSPI_INIT_READ_STATUS1:
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_read_status(self, &self->sensor_status, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_HWSWRESET:
			fp_dbg("<init> got status %02x", self->sensor_status);
			elanspi_do_hwreset(self, &err);
			fp_dbg("<init> sync hw reset");
			if (err) {
				fp_err("<init> sync hw reset failed");
				fpi_ssm_mark_failed(ssm, err);
				return;
			}
do_sw_reset:
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_do_swreset(self, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_READ_HEIGHT:
			fp_dbg("<init> sw reset ok");
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_read_height(self, &self->sensor_height, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_READ_WIDTH:
			fp_dbg("<init> raw height = %d", self->sensor_height);
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_read_width(self, &self->sensor_width, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_READ_REG17:
			fp_dbg("<init> raw width = %d", self->sensor_width);
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_read_register(self, 0x17, &self->sensor_reg_17, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_READ_VERSION:
			fp_dbg("<init> raw reg17 = %d", self->sensor_reg_17);
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_read_version(self, &self->sensor_raw_version, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_SWRESET2:
			fp_dbg("<init> raw version = %02x", self->sensor_raw_version);
			elanspi_determine_sensor(self, &err);
			if (err) {
				fp_err("<init> sensor detection error");
				fpi_ssm_mark_failed(ssm, err);
				return;
			}
			// reset again
			goto do_sw_reset;
		case ELANSPI_INIT_OTP_READ_VREF1:
			// is this sensor otp?
			if (!self->sensor_otp) {
				// go to calibration
				fpi_ssm_jump_to_state(ssm, ELANSPI_INIT_CALIBRATE);
				return;
			}
			// otherwise, begin otp
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_read_register(self, 0x3d, &self->sensor_reg_vref1, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_OTP_WRITE_VREF1:
			// mask out low bits
			self->sensor_reg_vref1 &= 0x3f;
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_write_register(self, 0x3d, self->sensor_reg_vref1, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_OTP_WRITE_0x28:
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_write_register(self, 0x28, 0x78, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		// begin loop
		case ELANSPI_INIT_OTP_LOOP_READ_0x28:
			// begin read of 0x28
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_read_register(self, 0x28, &self->sensor_reg_28, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_OTP_LOOP_READ_0x27:
			if (self->sensor_reg_28 & 0x40) {
				// try again
				fp_dbg("<init/otp> looping");
				fpi_ssm_jump_to_state(ssm, ELANSPI_INIT_OTP_LOOP_READ_0x28);
				return;
			}
			// otherwise, read reg 27
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_read_register(self, 0x27, &self->sensor_reg_27, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_OTP_LOOP_UPDATEDAC_READ_DAC2:
			// if high bit set, exit with mode 2
			if (self->sensor_reg_27 & 0x80) {
				self->sensor_vcm_mode = 2;
				fpi_ssm_jump_to_state(ssm, ELANSPI_INIT_OTP_WRITE_0xb);
				return;
			}
			// if low two bits are not set, loop
			if ((self->sensor_reg_27 & 6) != 6) {
				// try again
				fp_dbg("<init/otp> looping");
				fpi_ssm_jump_to_state(ssm, ELANSPI_INIT_OTP_LOOP_READ_0x28);
				return;
			}
			// otherwise, set vcm mode from low bit and read dac2
			self->sensor_vcm_mode = self->sensor_reg_27 & 1;
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_read_register(self, 0x7, &self->sensor_reg_dac2, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_OTP_LOOP_UPDATEDAC_WRITE_DAC2:
			// set high bit and rewrite
			self->sensor_reg_dac2 |= 0x80;
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_write_register(self, 0x7, self->sensor_reg_dac2, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_OTP_LOOP_UPDATEDAC_WRITE_10:
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_write_register(self, 0xa, 0x97, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		// end loop, joins to here on early exits
		case ELANSPI_INIT_OTP_WRITE_0xb:
			fp_dbg("<init/otp> got vcm mode = %d", self->sensor_vcm_mode);
			// if mode is 0, skip to calibration
			if (self->sensor_vcm_mode == 0) {
				fpi_ssm_jump_to_state(ssm, ELANSPI_INIT_CALIBRATE);
				return;
			}
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_write_register(self, 0xb, self->sensor_vcm_mode == 2 ? 0x72 : 0x71, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_OTP_WRITE_0xc:
			xfer = fpi_spi_transfer_new(dev, self->spi_fd);
			xfer->ssm = ssm;
			elanspi_write_register(self, 0xc, self->sensor_vcm_mode == 2 ? 0x62 : 0x49, xfer);
			fpi_spi_transfer_submit(xfer, fpi_device_get_cancellable(dev), fpi_ssm_spi_transfer_cb, NULL);
			return;
		case ELANSPI_INIT_CALIBRATE:
			fp_dbg("<init/calibrate> starting calibrate");
			// if sensor is hv
			if (self->sensor_id == 0xe) {
				chld = fpi_ssm_new(dev, elanspi_calibrate_hv_handler, ELANSPI_CALIBHV_NSTATES);
			}
			else {
				chld = fpi_ssm_new(dev, elanspi_calibrate_old_handler, ELANSPI_CALIBHV_NSTATES);
			}
			fpi_ssm_start_subsm(ssm, chld);
			return;
	}
}

static void dev_open(FpImageDevice *dev) {
	FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI(dev);
	GError *err = NULL;

	G_DEBUG_HERE();

	int spi_fd = open(fpi_device_get_udev_data(FP_DEVICE(dev), FP_DEVICE_UDEV_SUBTYPE_SPIDEV), O_RDWR);
	if (spi_fd < 0) {
		g_set_error(&err, G_IO_ERROR, g_io_error_from_errno(errno), "unable to open spi");
		fpi_image_device_open_complete(dev, err);
		return;
	}

	self->spi_fd = spi_fd;

	fpi_image_device_open_complete(dev, NULL);
}

static void dev_close(FpImageDevice *dev) {
	FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI(dev);

	if (self->spi_fd >= 0) {
		self->spi_fd = -1;
		close(self->spi_fd);
	}
	fpi_image_device_close_complete(dev, NULL);
}

static void fpi_device_elanspi_init(FpiDeviceElanSpi *self) {
	self->spi_fd = -1;
	self->sensor_id = 0xff;
	self->bg_image = NULL;
}

static void fpi_device_elanspi_finalize(GObject *this) {
	FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI(this);

	g_clear_pointer(&self->bg_image, g_free);

	G_OBJECT_CLASS(fpi_device_elanspi_parent_class)->finalize(this);
}

static void fpi_device_elanspi_class_init(FpiDeviceElanSpiClass *klass) {
	FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
	FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

	dev_class->id = "elanspi";
	dev_class->full_name = "ElanTech Embedded Fingerprint Sensor";
	dev_class->type = FP_DEVICE_TYPE_UDEV;
	dev_class->id_table = elanspi_id_table;
	dev_class->scan_type = FP_SCAN_TYPE_SWIPE;
	dev_class->nr_enroll_stages = 7; // these sensors are very hit or miss, may as well record a few extras

	img_class->bz3_threshold = 10;
	img_class->img_open = dev_open;
	img_class->activate = dev_activate;
	img_class->deactivate = dev_deactivate;
	img_class->change_state = dev_change_state;
	img_class->img_close = dev_close;

	G_OBJECT_CLASS(klass)->finalize = fpi_device_elanspi_finalize;
}
