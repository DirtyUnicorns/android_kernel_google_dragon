/*
 * Copyright © 2009 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/i2c.h>
#include <linux/seq_file.h>
#include <drm/drm_dp_helper.h>
#include <drm/drmP.h>

/**
 * DOC: dp helpers
 *
 * These functions contain some common logic and helpers at various abstraction
 * levels to deal with Display Port sink devices and related things like DP aux
 * channel transfers, EDID reading over DP aux channels, decoding certain DPCD
 * blocks, ...
 */

/* Run a single AUX_CH I2C transaction, writing/reading data as necessary */
static int
i2c_algo_dp_aux_transaction(struct i2c_adapter *adapter, int mode,
			    uint8_t write_byte, uint8_t *read_byte)
{
	struct i2c_algo_dp_aux_data *algo_data = adapter->algo_data;
	int ret;

	ret = (*algo_data->aux_ch)(adapter, mode,
				   write_byte, read_byte);
	return ret;
}

/*
 * I2C over AUX CH
 */

/*
 * Send the address. If the I2C link is running, this 'restarts'
 * the connection with the new address, this is used for doing
 * a write followed by a read (as needed for DDC)
 */
static int
i2c_algo_dp_aux_address(struct i2c_adapter *adapter, u16 address, bool reading)
{
	struct i2c_algo_dp_aux_data *algo_data = adapter->algo_data;
	int mode = MODE_I2C_START;
	int ret;

	if (reading)
		mode |= MODE_I2C_READ;
	else
		mode |= MODE_I2C_WRITE;
	algo_data->address = address;
	algo_data->running = true;
	ret = i2c_algo_dp_aux_transaction(adapter, mode, 0, NULL);
	return ret;
}

/*
 * Stop the I2C transaction. This closes out the link, sending
 * a bare address packet with the MOT bit turned off
 */
static void
i2c_algo_dp_aux_stop(struct i2c_adapter *adapter, bool reading)
{
	struct i2c_algo_dp_aux_data *algo_data = adapter->algo_data;
	int mode = MODE_I2C_STOP;

	if (reading)
		mode |= MODE_I2C_READ;
	else
		mode |= MODE_I2C_WRITE;
	if (algo_data->running) {
		(void) i2c_algo_dp_aux_transaction(adapter, mode, 0, NULL);
		algo_data->running = false;
	}
}

/*
 * Write a single byte to the current I2C address, the
 * the I2C link must be running or this returns -EIO
 */
static int
i2c_algo_dp_aux_put_byte(struct i2c_adapter *adapter, u8 byte)
{
	struct i2c_algo_dp_aux_data *algo_data = adapter->algo_data;
	int ret;

	if (!algo_data->running)
		return -EIO;

	ret = i2c_algo_dp_aux_transaction(adapter, MODE_I2C_WRITE, byte, NULL);
	return ret;
}

/*
 * Read a single byte from the current I2C address, the
 * I2C link must be running or this returns -EIO
 */
static int
i2c_algo_dp_aux_get_byte(struct i2c_adapter *adapter, u8 *byte_ret)
{
	struct i2c_algo_dp_aux_data *algo_data = adapter->algo_data;
	int ret;

	if (!algo_data->running)
		return -EIO;

	ret = i2c_algo_dp_aux_transaction(adapter, MODE_I2C_READ, 0, byte_ret);
	return ret;
}

static int
i2c_algo_dp_aux_xfer(struct i2c_adapter *adapter,
		     struct i2c_msg *msgs,
		     int num)
{
	int ret = 0;
	bool reading = false;
	int m;
	int b;

	for (m = 0; m < num; m++) {
		u16 len = msgs[m].len;
		u8 *buf = msgs[m].buf;
		reading = (msgs[m].flags & I2C_M_RD) != 0;
		ret = i2c_algo_dp_aux_address(adapter, msgs[m].addr, reading);
		if (ret < 0)
			break;
		if (reading) {
			for (b = 0; b < len; b++) {
				ret = i2c_algo_dp_aux_get_byte(adapter, &buf[b]);
				if (ret < 0)
					break;
			}
		} else {
			for (b = 0; b < len; b++) {
				ret = i2c_algo_dp_aux_put_byte(adapter, buf[b]);
				if (ret < 0)
					break;
			}
		}
		if (ret < 0)
			break;
	}
	if (ret >= 0)
		ret = num;
	i2c_algo_dp_aux_stop(adapter, reading);
	DRM_DEBUG_KMS("dp_aux_xfer return %d\n", ret);
	return ret;
}

static u32
i2c_algo_dp_aux_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL |
	       I2C_FUNC_SMBUS_READ_BLOCK_DATA |
	       I2C_FUNC_SMBUS_BLOCK_PROC_CALL |
	       I2C_FUNC_10BIT_ADDR;
}

static const struct i2c_algorithm i2c_dp_aux_algo = {
	.master_xfer	= i2c_algo_dp_aux_xfer,
	.functionality	= i2c_algo_dp_aux_functionality,
};

static void
i2c_dp_aux_reset_bus(struct i2c_adapter *adapter)
{
	(void) i2c_algo_dp_aux_address(adapter, 0, false);
	(void) i2c_algo_dp_aux_stop(adapter, false);
}

static int
i2c_dp_aux_prepare_bus(struct i2c_adapter *adapter)
{
	adapter->algo = &i2c_dp_aux_algo;
	adapter->retries = 3;
	i2c_dp_aux_reset_bus(adapter);
	return 0;
}

/**
 * i2c_dp_aux_add_bus() - register an i2c adapter using the aux ch helper
 * @adapter: i2c adapter to register
 *
 * This registers an i2c adapter that uses dp aux channel as it's underlaying
 * transport. The driver needs to fill out the &i2c_algo_dp_aux_data structure
 * and store it in the algo_data member of the @adapter argument. This will be
 * used by the i2c over dp aux algorithm to drive the hardware.
 *
 * RETURNS:
 * 0 on success, -ERRNO on failure.
 *
 * IMPORTANT:
 * This interface is deprecated, please switch to the new dp aux helpers and
 * drm_dp_aux_register().
 */
int
i2c_dp_aux_add_bus(struct i2c_adapter *adapter)
{
	int error;

	error = i2c_dp_aux_prepare_bus(adapter);
	if (error)
		return error;
	error = i2c_add_adapter(adapter);
	return error;
}
EXPORT_SYMBOL(i2c_dp_aux_add_bus);

/* Helpers for DP link training */
static u8 dp_link_status(const u8 link_status[DP_LINK_STATUS_SIZE], int r)
{
	return link_status[r - DP_LANE0_1_STATUS];
}

static u8 dp_get_lane_status(const u8 link_status[DP_LINK_STATUS_SIZE],
			     int lane)
{
	int i = DP_LANE0_1_STATUS + (lane >> 1);
	int s = (lane & 1) * 4;
	u8 l = dp_link_status(link_status, i);
	return (l >> s) & 0xf;
}

bool drm_dp_channel_eq_ok(const u8 link_status[DP_LINK_STATUS_SIZE],
			  int lane_count)
{
	u8 lane_align;
	u8 lane_status;
	int lane;

	lane_align = dp_link_status(link_status,
				    DP_LANE_ALIGN_STATUS_UPDATED);
	if ((lane_align & DP_INTERLANE_ALIGN_DONE) == 0)
		return false;
	for (lane = 0; lane < lane_count; lane++) {
		lane_status = dp_get_lane_status(link_status, lane);
		if ((lane_status & DP_CHANNEL_EQ_BITS) != DP_CHANNEL_EQ_BITS)
			return false;
	}
	return true;
}
EXPORT_SYMBOL(drm_dp_channel_eq_ok);

bool drm_dp_clock_recovery_ok(const u8 link_status[DP_LINK_STATUS_SIZE],
			      int lane_count)
{
	int lane;
	u8 lane_status;

	for (lane = 0; lane < lane_count; lane++) {
		lane_status = dp_get_lane_status(link_status, lane);
		if ((lane_status & DP_LANE_CR_DONE) == 0)
			return false;
	}
	return true;
}
EXPORT_SYMBOL(drm_dp_clock_recovery_ok);

u8 drm_dp_get_adjust_request_voltage(const u8 link_status[DP_LINK_STATUS_SIZE],
				     int lane)
{
	int i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	int s = ((lane & 1) ?
		 DP_ADJUST_VOLTAGE_SWING_LANE1_SHIFT :
		 DP_ADJUST_VOLTAGE_SWING_LANE0_SHIFT);
	u8 l = dp_link_status(link_status, i);

	return ((l >> s) & 0x3) << DP_TRAIN_VOLTAGE_SWING_SHIFT;
}
EXPORT_SYMBOL(drm_dp_get_adjust_request_voltage);

u8 drm_dp_get_adjust_request_pre_emphasis(const u8 link_status[DP_LINK_STATUS_SIZE],
					  int lane)
{
	int i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	int s = ((lane & 1) ?
		 DP_ADJUST_PRE_EMPHASIS_LANE1_SHIFT :
		 DP_ADJUST_PRE_EMPHASIS_LANE0_SHIFT);
	u8 l = dp_link_status(link_status, i);

	return ((l >> s) & 0x3) << DP_TRAIN_PRE_EMPHASIS_SHIFT;
}
EXPORT_SYMBOL(drm_dp_get_adjust_request_pre_emphasis);

u8 drm_dp_get_adjust_request_post_cursor(const u8 link_status[DP_LINK_STATUS_SIZE],
					 unsigned int lane)
{
	unsigned int offset = DP_ADJUST_REQUEST_POST_CURSOR2;
	u8 value = dp_link_status(link_status, offset);

	return (value >> (lane << 1)) & 0x3;
}
EXPORT_SYMBOL(drm_dp_get_adjust_request_post_cursor);

void drm_dp_link_train_clock_recovery_delay(const u8 dpcd[DP_RECEIVER_CAP_SIZE]) {
	if (dpcd[DP_TRAINING_AUX_RD_INTERVAL] == 0)
		udelay(100);
	else
		mdelay(dpcd[DP_TRAINING_AUX_RD_INTERVAL] * 4);
}
EXPORT_SYMBOL(drm_dp_link_train_clock_recovery_delay);

void drm_dp_link_train_channel_eq_delay(const u8 dpcd[DP_RECEIVER_CAP_SIZE]) {
	if (dpcd[DP_TRAINING_AUX_RD_INTERVAL] == 0)
		udelay(400);
	else
		mdelay(dpcd[DP_TRAINING_AUX_RD_INTERVAL] * 4);
}
EXPORT_SYMBOL(drm_dp_link_train_channel_eq_delay);

u8 drm_dp_link_rate_to_bw_code(int link_rate)
{
	switch (link_rate) {
	case 162000:
	default:
		return DP_LINK_BW_1_62;
	case 270000:
		return DP_LINK_BW_2_7;
	case 540000:
		return DP_LINK_BW_5_4;
	}
}
EXPORT_SYMBOL(drm_dp_link_rate_to_bw_code);

int drm_dp_bw_code_to_link_rate(u8 link_bw)
{
	switch (link_bw) {
	case DP_LINK_BW_1_62:
	default:
		return 162000;
	case DP_LINK_BW_2_7:
		return 270000;
	case DP_LINK_BW_5_4:
		return 540000;
	}
}
EXPORT_SYMBOL(drm_dp_bw_code_to_link_rate);

/**
 * DOC: dp helpers
 *
 * The DisplayPort AUX channel is an abstraction to allow generic, driver-
 * independent access to AUX functionality. Drivers can take advantage of
 * this by filling in the fields of the drm_dp_aux structure.
 *
 * Transactions are described using a hardware-independent drm_dp_aux_msg
 * structure, which is passed into a driver's .transfer() implementation.
 * Both native and I2C-over-AUX transactions are supported.
 */

static int drm_dp_dpcd_access(struct drm_dp_aux *aux, u8 request,
			      unsigned int offset, void *buffer, size_t size)
{
	struct drm_dp_aux_msg msg;
	unsigned int retry;
	int err;

	memset(&msg, 0, sizeof(msg));
	msg.address = offset;
	msg.request = request;
	msg.buffer = buffer;
	msg.size = size;

	/*
	 * The specification doesn't give any recommendation on how often to
	 * retry native transactions, so retry 7 times like for I2C-over-AUX
	 * transactions.
	 */
	for (retry = 0; retry < 7; retry++) {

		mutex_lock(&aux->hw_mutex);
		err = aux->transfer(aux, &msg);
		mutex_unlock(&aux->hw_mutex);
		if (err < 0) {
			if (err == -EBUSY)
				continue;

			return err;
		}


		switch (msg.reply & DP_AUX_NATIVE_REPLY_MASK) {
		case DP_AUX_NATIVE_REPLY_ACK:
			if (err < size)
				return -EPROTO;
			return err;

		case DP_AUX_NATIVE_REPLY_NACK:
			return -EIO;

		case DP_AUX_NATIVE_REPLY_DEFER:
			usleep_range(400, 500);
			break;
		}
	}

	DRM_DEBUG_KMS("too many retries, giving up\n");
	return -EIO;
}

/**
 * drm_dp_dpcd_read() - read a series of bytes from the DPCD
 * @aux: DisplayPort AUX channel
 * @offset: address of the (first) register to read
 * @buffer: buffer to store the register values
 * @size: number of bytes in @buffer
 *
 * Returns the number of bytes transferred on success, or a negative error
 * code on failure. -EIO is returned if the request was NAKed by the sink or
 * if the retry count was exceeded. If not all bytes were transferred, this
 * function returns -EPROTO. Errors from the underlying AUX channel transfer
 * function, with the exception of -EBUSY (which causes the transaction to
 * be retried), are propagated to the caller.
 */
ssize_t drm_dp_dpcd_read(struct drm_dp_aux *aux, unsigned int offset,
			 void *buffer, size_t size)
{
	return drm_dp_dpcd_access(aux, DP_AUX_NATIVE_READ, offset, buffer,
				  size);
}
EXPORT_SYMBOL(drm_dp_dpcd_read);

/**
 * drm_dp_dpcd_write() - write a series of bytes to the DPCD
 * @aux: DisplayPort AUX channel
 * @offset: address of the (first) register to write
 * @buffer: buffer containing the values to write
 * @size: number of bytes in @buffer
 *
 * Returns the number of bytes transferred on success, or a negative error
 * code on failure. -EIO is returned if the request was NAKed by the sink or
 * if the retry count was exceeded. If not all bytes were transferred, this
 * function returns -EPROTO. Errors from the underlying AUX channel transfer
 * function, with the exception of -EBUSY (which causes the transaction to
 * be retried), are propagated to the caller.
 */
ssize_t drm_dp_dpcd_write(struct drm_dp_aux *aux, unsigned int offset,
			  void *buffer, size_t size)
{
	return drm_dp_dpcd_access(aux, DP_AUX_NATIVE_WRITE, offset, buffer,
				  size);
}
EXPORT_SYMBOL(drm_dp_dpcd_write);

/**
 * drm_dp_dpcd_read_link_status() - read DPCD link status (bytes 0x202-0x207)
 * @aux: DisplayPort AUX channel
 * @status: buffer to store the link status in (must be at least 6 bytes)
 *
 * Returns the number of bytes transferred on success or a negative error
 * code on failure.
 */
int drm_dp_dpcd_read_link_status(struct drm_dp_aux *aux,
				 u8 status[DP_LINK_STATUS_SIZE])
{
	return drm_dp_dpcd_read(aux, DP_LANE0_1_STATUS, status,
				DP_LINK_STATUS_SIZE);
}
EXPORT_SYMBOL(drm_dp_dpcd_read_link_status);

/**
 * drm_dp_dpcd_dump() - dump DPCD content
 * @aux: DisplayPort AUX channel
 * @s: destination for DPCD dump
 *
 * Reads registers from the DPCD via a DisplayPort AUX channel and dumps them
 * to a seq_file.
 */
void drm_dp_dpcd_dump(struct drm_dp_aux *aux, struct seq_file *s)
{
#define DUMP_REG(aux, offset) ({					\
		u8 value;						\
		int err;						\
		err = drm_dp_dpcd_readb(aux, offset, &value);		\
		if (err < 0) {						\
			dev_err((aux)->dev, "failed to read %s: %d\n",	\
				#offset, err);				\
			return;						\
		}							\
		seq_printf(s, "%-35s 0x%04x 0x%02x\n", #offset, offset,	\
			   value);					\
	})

	DUMP_REG(aux, DP_DPCD_REV);
	DUMP_REG(aux, DP_MAX_LINK_RATE);
	DUMP_REG(aux, DP_MAX_LANE_COUNT);
	DUMP_REG(aux, DP_MAX_DOWNSPREAD);
	DUMP_REG(aux, DP_NORP);
	DUMP_REG(aux, DP_DOWNSTREAMPORT_PRESENT);
	DUMP_REG(aux, DP_MAIN_LINK_CHANNEL_CODING);
	DUMP_REG(aux, DP_DOWN_STREAM_PORT_COUNT);
	DUMP_REG(aux, DP_RECEIVE_PORT_0_CAP_0);
	DUMP_REG(aux, DP_RECEIVE_PORT_0_BUFFER_SIZE);
	DUMP_REG(aux, DP_RECEIVE_PORT_1_CAP_0);
	DUMP_REG(aux, DP_RECEIVE_PORT_1_BUFFER_SIZE);
	DUMP_REG(aux, DP_I2C_SPEED_CAP);
	DUMP_REG(aux, DP_EDP_CONFIGURATION_CAP);
	DUMP_REG(aux, DP_TRAINING_AUX_RD_INTERVAL);
	DUMP_REG(aux, DP_ADAPTER_CAP);
	DUMP_REG(aux, DP_SUPPORTED_LINK_RATES);
	DUMP_REG(aux, DP_FAUX_CAP);
	DUMP_REG(aux, DP_MSTM_CAP);
	DUMP_REG(aux, DP_NUMBER_OF_AUDIO_ENDPOINTS);
	DUMP_REG(aux, DP_AV_GRANULARITY);
	DUMP_REG(aux, DP_AUD_DEC_LAT0);
	DUMP_REG(aux, DP_AUD_DEC_LAT1);
	DUMP_REG(aux, DP_AUD_PP_LAT0);
	DUMP_REG(aux, DP_AUD_PP_LAT1);
	DUMP_REG(aux, DP_VID_INTER_LAT);
	DUMP_REG(aux, DP_VID_PROG_LAT);
	DUMP_REG(aux, DP_REP_LAT);
	DUMP_REG(aux, DP_AUD_DEL_INS0);
	DUMP_REG(aux, DP_AUD_DEL_INS1);
	DUMP_REG(aux, DP_AUD_DEL_INS2);
	DUMP_REG(aux, DP_RECEIVER_ALPM_CAP);
	DUMP_REG(aux, DP_AUD_DEL_INS0);
	DUMP_REG(aux, DP_GUID);
	DUMP_REG(aux, DP_PSR_SUPPORT);
	DUMP_REG(aux, DP_PSR_CAPS);
	DUMP_REG(aux, DP_DOWNSTREAM_PORT_0);
	DUMP_REG(aux, DP_LINK_BW_SET);
	DUMP_REG(aux, DP_LANE_COUNT_SET);
	DUMP_REG(aux, DP_TRAINING_PATTERN_SET);
	DUMP_REG(aux, DP_TRAINING_LANE0_SET);
	DUMP_REG(aux, DP_TRAINING_LANE1_SET);
	DUMP_REG(aux, DP_TRAINING_LANE2_SET);
	DUMP_REG(aux, DP_TRAINING_LANE3_SET);
	DUMP_REG(aux, DP_DOWNSPREAD_CTRL);
	DUMP_REG(aux, DP_MAIN_LINK_CHANNEL_CODING_SET);
	DUMP_REG(aux, DP_I2C_SPEED_CONTROL_STATUS);
	DUMP_REG(aux, DP_EDP_CONFIGURATION_SET);
	DUMP_REG(aux, DP_LINK_QUAL_LANE0_SET);
	DUMP_REG(aux, DP_LINK_QUAL_LANE1_SET);
	DUMP_REG(aux, DP_LINK_QUAL_LANE2_SET);
	DUMP_REG(aux, DP_LINK_QUAL_LANE3_SET);
	DUMP_REG(aux, DP_TRAINING_LANE0_1_SET2);
	DUMP_REG(aux, DP_TRAINING_LANE2_3_SET2);
	DUMP_REG(aux, DP_MSTM_CTRL);
	DUMP_REG(aux, DP_AUDIO_DELAY0);
	DUMP_REG(aux, DP_AUDIO_DELAY1);
	DUMP_REG(aux, DP_AUDIO_DELAY2);
	DUMP_REG(aux, DP_LINK_RATE_SET);
	DUMP_REG(aux, DP_RECEIVER_ALPM_CONFIG);
	DUMP_REG(aux, DP_SINK_DEVICE_AUX_FRAME_SYNC_CONF);
	DUMP_REG(aux, DP_UPSTREAM_DEVICE_DP_PWR_NEED);
	DUMP_REG(aux, DP_AUX_FRAME_SYNC_VALUE);
	DUMP_REG(aux, DP_PSR_EN_CFG);
	DUMP_REG(aux, DP_ADAPTER_CTRL);
	DUMP_REG(aux, DP_BRANCH_DEVICE_CTRL);
	DUMP_REG(aux, DP_PAYLOAD_ALLOCATE_SET);
	DUMP_REG(aux, DP_PAYLOAD_ALLOCATE_START_TIME_SLOT);
	DUMP_REG(aux, DP_PAYLOAD_ALLOCATE_TIME_SLOT_COUNT);
	DUMP_REG(aux, DP_SINK_COUNT);
	DUMP_REG(aux, DP_DEVICE_SERVICE_IRQ_VECTOR);
	DUMP_REG(aux, DP_LANE0_1_STATUS);
	DUMP_REG(aux, DP_LANE2_3_STATUS);
	DUMP_REG(aux, DP_LANE_ALIGN_STATUS_UPDATED);
	DUMP_REG(aux, DP_SINK_STATUS);
	DUMP_REG(aux, DP_ADJUST_REQUEST_LANE0_1);
	DUMP_REG(aux, DP_ADJUST_REQUEST_LANE2_3);
	DUMP_REG(aux, DP_TEST_REQUEST);
	DUMP_REG(aux, DP_TEST_LINK_RATE);
	DUMP_REG(aux, DP_TEST_LANE_COUNT);
	DUMP_REG(aux, DP_TEST_CRC_R_CR);
	DUMP_REG(aux, DP_TEST_CRC_G_Y);
	DUMP_REG(aux, DP_TEST_CRC_B_CB);
	DUMP_REG(aux, DP_TEST_SINK_MISC);
	DUMP_REG(aux, DP_TEST_RESPONSE);
	DUMP_REG(aux, DP_TEST_EDID_CHECKSUM);
	DUMP_REG(aux, DP_TEST_SINK);
	DUMP_REG(aux, DP_PAYLOAD_TABLE_UPDATE_STATUS);
	DUMP_REG(aux, DP_VC_PAYLOAD_ID_SLOT_1);
	DUMP_REG(aux, DP_SOURCE_OUI);
	DUMP_REG(aux, DP_SINK_OUI);
	DUMP_REG(aux, DP_BRANCH_OUI);
	DUMP_REG(aux, DP_SET_POWER);
	DUMP_REG(aux, DP_EDP_DPCD_REV);
	DUMP_REG(aux, DP_EDP_GENERAL_CAP_1);
	DUMP_REG(aux, DP_EDP_BACKLIGHT_ADJUSTMENT_CAP);
	DUMP_REG(aux, DP_EDP_GENERAL_CAP_2);
	DUMP_REG(aux, DP_EDP_GENERAL_CAP_3);
	DUMP_REG(aux, DP_EDP_DISPLAY_CONTROL_REGISTER);
	DUMP_REG(aux, DP_EDP_BACKLIGHT_MODE_SET_REGISTER);
	DUMP_REG(aux, DP_EDP_BACKLIGHT_BRIGHTNESS_MSB);
	DUMP_REG(aux, DP_EDP_BACKLIGHT_BRIGHTNESS_LSB);
	DUMP_REG(aux, DP_EDP_PWMGEN_BIT_COUNT);
	DUMP_REG(aux, DP_EDP_PWMGEN_BIT_COUNT_CAP_MIN);
	DUMP_REG(aux, DP_EDP_PWMGEN_BIT_COUNT_CAP_MAX);
	DUMP_REG(aux, DP_EDP_BACKLIGHT_CONTROL_STATUS);
	DUMP_REG(aux, DP_EDP_BACKLIGHT_FREQ_SET);
	DUMP_REG(aux, DP_EDP_BACKLIGHT_FREQ_CAP_MIN_MSB);
	DUMP_REG(aux, DP_EDP_BACKLIGHT_FREQ_CAP_MIN_MID);
	DUMP_REG(aux, DP_EDP_BACKLIGHT_FREQ_CAP_MIN_LSB);
	DUMP_REG(aux, DP_EDP_BACKLIGHT_FREQ_CAP_MAX_MSB);
	DUMP_REG(aux, DP_EDP_BACKLIGHT_FREQ_CAP_MAX_MID);
	DUMP_REG(aux, DP_EDP_BACKLIGHT_FREQ_CAP_MAX_LSB);
	DUMP_REG(aux, DP_EDP_DBC_MINIMUM_BRIGHTNESS_SET);
	DUMP_REG(aux, DP_EDP_DBC_MAXIMUM_BRIGHTNESS_SET);
	DUMP_REG(aux, DP_EDP_REGIONAL_BACKLIGHT_BASE);
	DUMP_REG(aux, DP_EDP_REGIONAL_BACKLIGHT_0);

#undef DUMP_REG
}
EXPORT_SYMBOL(drm_dp_dpcd_dump);

static void drm_dp_link_reset(struct drm_dp_link *link)
{
	if (!link)
		return;

	link->revision = 0;
	link->edp = 0;
	link->max_lanes = 0;
	link->max_rate = 0;
	link->capabilities = 0;
	link->aux_rd_interval = 0;

	link->lanes = 0;
	link->rate = 0;
}

/**
 * drm_dp_link_probe() - probe a DisplayPort link for capabilities
 * @aux: DisplayPort AUX channel
 * @link: pointer to structure in which to return link capabilities
 *
 * The structure filled in by this function can usually be passed directly
 * into drm_dp_link_power_up() and drm_dp_link_configure() to power up and
 * configure the link based on the link's capabilities.
 *
 * Returns 0 on success or a negative error code on failure.
 */
int drm_dp_link_probe(struct drm_dp_aux *aux, struct drm_dp_link *link)
{
	u8 values[16], value;
	int err;

	drm_dp_link_reset(link);

	err = drm_dp_dpcd_read(aux, DP_DPCD_REV, values, sizeof(values));
	if (err < 0)
		return err;

	link->revision = values[0];
	link->max_rate = drm_dp_bw_code_to_link_rate(values[1]);
	link->max_lanes = values[2] & DP_MAX_LANE_COUNT_MASK;

	if (link->revision >= 0x11)
		if (values[2] & DP_ENHANCED_FRAME_CAP)
			link->capabilities |= DP_LINK_CAP_ENHANCED_FRAMING;

	if (link->revision >= 0x12)
		if (values[2] & DP_TPS3_SUPPORTED)
			link->capabilities |= DP_LINK_CAP_TPS3;

	if (link->revision >= 0x11)
		if (values[3] & DP_NO_AUX_HANDSHAKE_LINK_TRAINING)
			link->capabilities |= DP_LINK_CAP_FAST_TRAINING;

	if (link->revision >= 0x12) {
		if (values[6] & DP_SET_ANSI_8B10B)
			link->capabilities |= DP_LINK_CAP_ANSI_8B10B;
	} else if (link->revision >= 0x11)
		link->capabilities |= DP_LINK_CAP_ANSI_8B10B;

	if (values[13] & DP_ALTERNATE_SCRAMBLER_RESET_CAP) {
		link->capabilities |= DP_LINK_CAP_ALTERNATE_SCRAMBLER_RESET;

		err = drm_dp_dpcd_readb(aux, DP_EDP_DPCD_REV, &value);
		if (err < 0)
			return err;

		switch (value) {
		case DP_EDP_11:
			link->edp = 0x11;
			break;

		case DP_EDP_12:
			link->edp = 0x12;
			break;

		case DP_EDP_13:
			link->edp = 0x13;
			break;

		case DP_EDP_14:
			link->edp = 0x14;
			break;

		default:
			DRM_ERROR("unsupported eDP version: %02x\n", value);
			break;
		}
	}

	/* DP_TRAINING_AUX_RD_INTERVAL is in units of 4 milliseconds */
	link->aux_rd_interval = values[14] * 4000;

	/* use highest available configuration by default */
	link->lanes = link->max_lanes;
	link->rate = link->max_rate;

	return 0;
}
EXPORT_SYMBOL(drm_dp_link_probe);

/**
 * drm_dp_link_power_up() - power up a DisplayPort link
 * @aux: DisplayPort AUX channel
 * @link: pointer to a structure containing the link configuration
 *
 * Returns 0 on success or a negative error code on failure.
 */
int drm_dp_link_power_up(struct drm_dp_aux *aux, struct drm_dp_link *link)
{
	u8 value;
	int err;

	/* DP_SET_POWER register is only available on DPCD v1.1 and later */
	if (link->revision < 0x11)
		return 0;

	err = drm_dp_dpcd_readb(aux, DP_SET_POWER, &value);
	if (err < 0)
		return err;

	value &= ~DP_SET_POWER_MASK;
	value |= DP_SET_POWER_D0;

	err = drm_dp_dpcd_writeb(aux, DP_SET_POWER, value);
	if (err < 0)
		return err;

	/*
	 * According to the DP 1.1 specification, a "Sink Device must exit the
	 * power saving state within 1 ms" (Section 2.5.3.1, Table 5-52, "Sink
	 * Control Field" (register 0x600).
	 */
	usleep_range(1000, 2000);

	return 0;
}
EXPORT_SYMBOL(drm_dp_link_power_up);

/**
 * drm_dp_link_power_down() - power down a DisplayPort link
 * @aux: DisplayPort AUX channel
 * @link: pointer to a structure containing the link configuration
 *
 * Returns 0 on success or a negative error code on failure.
 */
int drm_dp_link_power_down(struct drm_dp_aux *aux, struct drm_dp_link *link)
{
	u8 value;
	int err;

	/* DP_SET_POWER register is only available on DPCD v1.1 and later */
	if (link->revision < 0x11)
		return 0;

	err = drm_dp_dpcd_readb(aux, DP_SET_POWER, &value);
	if (err < 0)
		return err;

	value &= ~DP_SET_POWER_MASK;
	value |= DP_SET_POWER_D3;

	err = drm_dp_dpcd_writeb(aux, DP_SET_POWER, value);
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(drm_dp_link_power_down);

/**
 * drm_dp_link_configure() - configure a DisplayPort link
 * @aux: DisplayPort AUX channel
 * @link: pointer to a structure containing the link configuration
 *
 * Returns 0 on success or a negative error code on failure.
 */
int drm_dp_link_configure(struct drm_dp_aux *aux, struct drm_dp_link *link)
{
	u8 values[2], value = 0;
	int err;

	if (link->ops && link->ops->configure) {
		err = link->ops->configure(link);
		if (err < 0) {
			DRM_ERROR("failed to configure DP link: %d\n", err);
			return err;
		}
	}

	values[0] = drm_dp_link_rate_to_bw_code(link->rate);
	values[1] = link->lanes;

	if (link->capabilities & DP_LINK_CAP_ENHANCED_FRAMING)
		values[1] |= DP_LANE_COUNT_ENHANCED_FRAME_EN;

	err = drm_dp_dpcd_write(aux, DP_LINK_BW_SET, values, sizeof(values));
	if (err < 0)
		return err;

	if (link->capabilities & DP_LINK_CAP_ANSI_8B10B)
		value = DP_SET_ANSI_8B10B;
	else
		value = 0;

	err = drm_dp_dpcd_writeb(aux, DP_MAIN_LINK_CHANNEL_CODING_SET, value);
	if (err < 0)
		return err;

	if (link->capabilities & DP_LINK_CAP_ALTERNATE_SCRAMBLER_RESET) {
		err = drm_dp_dpcd_writeb(aux, DP_EDP_CONFIGURATION_SET,
					 DP_ALTERNATE_SCRAMBLER_RESET_ENABLE);
		if (err < 0)
			return err;
	}

	return 0;
}
EXPORT_SYMBOL(drm_dp_link_configure);

/**
 * drm_dp_link_choose() - choose the lowest possible configuration for a mode
 * @link: DRM DP link object
 * @mode: DRM display mode
 * @info: DRM display information
 *
 * According to the eDP specification, a source should select a configuration
 * with the lowest number of lanes and the lowest possible link rate that can
 * match the bitrate requirements of a video mode. However it must ensure not
 * to exceed the capabilities of the sink.
 *
 * Returns: 0 on success or a negative error code on failure.
 */
int drm_dp_link_choose(struct drm_dp_link *link,
		       const struct drm_display_mode *mode,
		       const struct drm_display_info *info)
{
	/* available link symbol clock rates */
	static const unsigned int rates[3] = { 162000, 270000, 540000 };
	/* available number of lanes */
	static const unsigned int lanes[3] = { 1, 2, 4 };
	unsigned long requirement, capacity;
	unsigned int rate = link->max_rate;
	unsigned int i, j;

	/* bandwidth requirement */
	requirement = mode->clock * info->bpc * 3;

	for (i = 0; i < ARRAY_SIZE(lanes) && lanes[i] <= link->max_lanes; i++) {
		for (j = 0; j < ARRAY_SIZE(rates) && rates[j] <= rate; j++) {
			/*
			 * Capacity for this combination of lanes and rate,
			 * factoring in the ANSI 8B/10B encoding.
			 *
			 * Link rates in the DRM DP helpers are really link
			 * symbol frequencies, so a tenth of the actual rate
			 * of the link.
			 */
			capacity = lanes[i] * (rates[j] * 10) * 8 / 10;

			if (capacity >= requirement) {
				DRM_DEBUG_KMS("using %u lanes at %u kHz (%lu/%lu kbps)\n",
					      lanes[i], rates[j], requirement,
					      capacity);
				link->lanes = lanes[i];
				link->rate = rates[j];
				return 0;
			}
		}
	}

	return -ERANGE;
}

/*
 * I2C-over-AUX implementation
 */

static u32 drm_dp_i2c_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL |
	       I2C_FUNC_SMBUS_READ_BLOCK_DATA |
	       I2C_FUNC_SMBUS_BLOCK_PROC_CALL |
	       I2C_FUNC_10BIT_ADDR;
}

/*
 * Transfer a single I2C-over-AUX message and handle various error conditions,
 * retrying the transaction as appropriate.  It is assumed that the
 * aux->transfer function does not modify anything in the msg other than the
 * reply field.
 */
static int drm_dp_i2c_do_msg(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg)
{
	unsigned int retry, defer_i2c;
	int err;

	/*
	 * DP1.2 sections 2.7.7.1.5.6.1 and 2.7.7.1.6.6.1: A DP Source device
	 * is required to retry at least seven times upon receiving AUX_DEFER
	 * before giving up the AUX transaction.
	 */
	for (retry = 0, defer_i2c = 0; retry < (7 + defer_i2c); retry++) {
		mutex_lock(&aux->hw_mutex);
		err = aux->transfer(aux, msg);
		mutex_unlock(&aux->hw_mutex);
		if (err < 0) {
			if (err == -EBUSY)
				continue;

			DRM_DEBUG_KMS("transaction failed: %d\n", err);
			return err;
		}


		switch (msg->reply & DP_AUX_NATIVE_REPLY_MASK) {
		case DP_AUX_NATIVE_REPLY_ACK:
			/*
			 * For I2C-over-AUX transactions this isn't enough, we
			 * need to check for the I2C ACK reply.
			 */
			break;

		case DP_AUX_NATIVE_REPLY_NACK:
			DRM_DEBUG_KMS("native nack\n");
			return -EREMOTEIO;

		case DP_AUX_NATIVE_REPLY_DEFER:
			DRM_DEBUG_KMS("native defer\n");
			/*
			 * We could check for I2C bit rate capabilities and if
			 * available adjust this interval. We could also be
			 * more careful with DP-to-legacy adapters where a
			 * long legacy cable may force very low I2C bit rates.
			 *
			 * For now just defer for long enough to hopefully be
			 * safe for all use-cases.
			 */
			usleep_range(500, 600);
			continue;

		default:
			DRM_ERROR("invalid native reply %#04x\n", msg->reply);
			return -EREMOTEIO;
		}

		switch (msg->reply & DP_AUX_I2C_REPLY_MASK) {
		case DP_AUX_I2C_REPLY_ACK:
			/*
			 * Both native ACK and I2C ACK replies received. We
			 * can assume the transfer was successful.
			 */
			if (err < msg->size)
				return -EPROTO;
			return 0;

		case DP_AUX_I2C_REPLY_NACK:
			DRM_DEBUG_KMS("I2C nack\n");
			aux->i2c_nack_count++;
			return -EREMOTEIO;

		case DP_AUX_I2C_REPLY_DEFER:
			DRM_DEBUG_KMS("I2C defer\n");
			/* DP Compliance Test 4.2.2.5 Requirement:
			 * Must have at least 7 retries for I2C defers on the
			 * transaction to pass this test
			 */
			aux->i2c_defer_count++;
			if (defer_i2c < 7)
				defer_i2c++;
			usleep_range(400, 500);
			continue;

		default:
			DRM_ERROR("invalid I2C reply %#04x\n", msg->reply);
			return -EREMOTEIO;
		}
	}

	DRM_DEBUG_KMS("too many retries, giving up\n");
	return -EREMOTEIO;
}

static int drm_dp_i2c_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs,
			   int num)
{
	struct drm_dp_aux *aux = adapter->algo_data;
	unsigned int i, j;
	struct drm_dp_aux_msg msg;
	int err = 0;

	memset(&msg, 0, sizeof(msg));

	for (i = 0; i < num; i++) {
		msg.address = msgs[i].addr;
		msg.request = (msgs[i].flags & I2C_M_RD) ?
			DP_AUX_I2C_READ :
			DP_AUX_I2C_WRITE;
		msg.request |= DP_AUX_I2C_MOT;
		/* Send a bare address packet to start the transaction.
		 * Zero sized messages specify an address only (bare
		 * address) transaction.
		 */
		msg.buffer = NULL;
		msg.size = 0;
		err = drm_dp_i2c_do_msg(aux, &msg);
		if (err < 0)
			DRM_INFO("Bare address transaction is failed: %d\n",
				err);
		/*
		 * Many hardware implementations support FIFOs larger than a
		 * single byte, but it has been empirically determined that
		 * transferring data in larger chunks can actually lead to
		 * decreased performance. Therefore each message is simply
		 * transferred byte-by-byte.
		 */
		for (j = 0; j < msgs[i].len; j++) {
			msg.buffer = msgs[i].buf + j;
			msg.size = 1;

			err = drm_dp_i2c_do_msg(aux, &msg);
			if (err < 0)
				break;
		}
		if (err < 0)
			break;
	}
	if (err >= 0)
		err = num;
	/* Send a bare address packet to close out the transaction.
	 * Zero sized messages specify an address only (bare
	 * address) transaction.
	 */
	msg.request &= ~DP_AUX_I2C_MOT;
	msg.buffer = NULL;
	msg.size = 0;
	(void)drm_dp_i2c_do_msg(aux, &msg);

	return err;
}

static const struct i2c_algorithm drm_dp_i2c_algo = {
	.functionality = drm_dp_i2c_functionality,
	.master_xfer = drm_dp_i2c_xfer,
};

/**
 * drm_dp_aux_register() - initialise and register aux channel
 * @aux: DisplayPort AUX channel
 *
 * Returns 0 on success or a negative error code on failure.
 */
int drm_dp_aux_register(struct drm_dp_aux *aux)
{
	mutex_init(&aux->hw_mutex);

	aux->ddc.algo = &drm_dp_i2c_algo;
	aux->ddc.algo_data = aux;
	aux->ddc.retries = 3;

	aux->ddc.class = I2C_CLASS_DDC;
	aux->ddc.owner = THIS_MODULE;
	aux->ddc.dev.parent = aux->dev;
	aux->ddc.dev.of_node = aux->dev->of_node;

	strlcpy(aux->ddc.name, aux->name ? aux->name : dev_name(aux->dev),
		sizeof(aux->ddc.name));

	return i2c_add_adapter(&aux->ddc);
}
EXPORT_SYMBOL(drm_dp_aux_register);

/**
 * drm_dp_aux_unregister() - unregister an AUX adapter
 * @aux: DisplayPort AUX channel
 */
void drm_dp_aux_unregister(struct drm_dp_aux *aux)
{
	i2c_del_adapter(&aux->ddc);
}
EXPORT_SYMBOL(drm_dp_aux_unregister);

/**
 * DOC: Link training
 *
 * These functions contain common logic and helpers to implement DisplayPort
 * link training.
 */

/**
 * drm_dp_link_train_init() - initialize DisplayPort link training state
 * @train: DisplayPort link training state
 */
void drm_dp_link_train_init(struct drm_dp_link_train *train)
{
	struct drm_dp_link_train_set *request = &train->request;
	struct drm_dp_link_train_set *adjust = &train->adjust;
	unsigned int i;

	for (i = 0; i < 4; i++) {
		request->voltage_swing[i] = 0;
		adjust->voltage_swing[i] = 0;

		request->pre_emphasis[i] = 0;
		adjust->pre_emphasis[i] = 0;

		request->post_cursor[i] = 0;
		adjust->post_cursor[i] = 0;
	}

	train->pattern = DP_TRAINING_PATTERN_DISABLE;
	train->clock_recovered = false;
	train->channel_equalized = false;
}

static bool drm_dp_link_train_valid(const struct drm_dp_link_train *train)
{
	return train->clock_recovered && train->channel_equalized;
}

static int drm_dp_link_apply_training(struct drm_dp_link *link)
{
	struct drm_dp_link_train_set *request = &link->train.request;
	unsigned int lanes = link->lanes, *vs, *pe, *pc, i;
	struct drm_dp_aux *aux = link->aux;
	u8 values[4], pattern = 0;
	int err;

	err = link->ops->apply_training(link);
	if (err < 0) {
		DRM_ERROR("failed to apply link training: %d\n", err);
		return err;
	}

	vs = request->voltage_swing;
	pe = request->pre_emphasis;
	pc = request->post_cursor;

	/* write currently selected voltage-swing and pre-emphasis levels */
	for (i = 0; i < lanes; i++)
		values[i] = DP_TRAIN_VOLTAGE_SWING_LEVEL(vs[i]) |
			    DP_TRAIN_PRE_EMPHASIS_LEVEL(pe[i]);

	err = drm_dp_dpcd_write(aux, DP_TRAINING_LANE0_SET, values, lanes);
	if (err < 0) {
		DRM_ERROR("failed to set training parameters: %d\n", err);
		return err;
	}

	/* write currently selected post-cursor level (if supported) */
	if (link->revision >= 0x12 && link->rate == 540000) {
		values[0] = values[1] = 0;

		for (i = 0; i < lanes; i++)
			values[i / 2] |= DP_LANE_POST_CURSOR(i, pc[i]);

		err = drm_dp_dpcd_write(aux, DP_TRAINING_LANE0_1_SET2, values,
					DIV_ROUND_UP(lanes, 2));
		if (err < 0) {
			DRM_ERROR("failed to set post-cursor: %d\n", err);
			return err;
		}
	}

	/* write link pattern */
	if (link->train.pattern != DP_TRAINING_PATTERN_DISABLE)
		pattern |= DP_LINK_SCRAMBLING_DISABLE;

	pattern |= link->train.pattern;

	err = drm_dp_dpcd_writeb(aux, DP_TRAINING_PATTERN_SET, pattern);
	if (err < 0) {
		DRM_ERROR("failed to set training pattern: %d\n", err);
		return err;
	}

	return 0;
}

static void drm_dp_link_train_wait(struct drm_dp_link *link)
{
	unsigned long min = 0;

	if (link->aux_rd_interval == 0) {
		switch (link->train.pattern) {
		case DP_TRAINING_PATTERN_1:
			min = 100;
			break;

		case DP_TRAINING_PATTERN_2:
		case DP_TRAINING_PATTERN_3:
			min = 400;
			break;

		default:
			break;
		}
	} else {
		min = link->aux_rd_interval;
	}

	if (min > 0)
		usleep_range(min, 2 * min);
}

static void drm_dp_link_get_adjustments(struct drm_dp_link *link,
					u8 status[DP_LINK_STATUS_SIZE])
{
	struct drm_dp_link_train_set *adjust = &link->train.adjust;
	unsigned int i;

	for (i = 0; i < link->lanes; i++) {
		adjust->voltage_swing[i] =
			drm_dp_get_adjust_request_voltage(status, i) >>
				DP_TRAIN_VOLTAGE_SWING_SHIFT;

		adjust->pre_emphasis[i] =
			drm_dp_get_adjust_request_pre_emphasis(status, i) >>
				DP_TRAIN_PRE_EMPHASIS_SHIFT;

		adjust->post_cursor[i] =
			drm_dp_get_adjust_request_post_cursor(status, i);
	}
}

static void drm_dp_link_train_adjust(struct drm_dp_link_train *train)
{
	struct drm_dp_link_train_set *request = &train->request;
	struct drm_dp_link_train_set *adjust = &train->adjust;
	unsigned int i;

	for (i = 0; i < 4; i++)
		if (request->voltage_swing[i] != adjust->voltage_swing[i])
			request->voltage_swing[i] = adjust->voltage_swing[i];

	for (i = 0; i < 4; i++)
		if (request->pre_emphasis[i] != adjust->pre_emphasis[i])
			request->pre_emphasis[i] = adjust->pre_emphasis[i];

	for (i = 0; i < 4; i++)
		if (request->post_cursor[i] != adjust->post_cursor[i])
			request->post_cursor[i] = adjust->post_cursor[i];
}

static int drm_dp_link_recover_clock(struct drm_dp_link *link)
{
	u8 status[DP_LINK_STATUS_SIZE];
	int err;

	err = drm_dp_link_apply_training(link);
	if (err < 0)
		return err;

	drm_dp_link_train_wait(link);

	err = drm_dp_dpcd_read_link_status(link->aux, status);
	if (err < 0) {
		DRM_ERROR("failed to read link status: %d\n", err);
		return err;
	}

	if (!drm_dp_clock_recovery_ok(status, link->lanes))
		drm_dp_link_get_adjustments(link, status);
	else
		link->train.clock_recovered = true;

	return 0;
}

static int drm_dp_link_clock_recovery(struct drm_dp_link *link)
{
	unsigned int repeat;
	int err;

	/* start clock recovery using training pattern 1 */
	link->train.pattern = DP_TRAINING_PATTERN_1;

	for (repeat = 1; repeat < 5; repeat++) {
		err = drm_dp_link_recover_clock(link);
		if (err < 0) {
			DRM_ERROR("failed to recover clock: %d\n", err);
			return err;
		}

		drm_dp_link_train_adjust(&link->train);

		if (link->train.clock_recovered)
			break;
	}

	return 0;
}

static int drm_dp_link_equalize_channel(struct drm_dp_link *link)
{
	struct drm_dp_aux *aux = link->aux;
	u8 status[DP_LINK_STATUS_SIZE];
	int err;

	err = drm_dp_link_apply_training(link);
	if (err < 0)
		return err;

	drm_dp_link_train_wait(link);

	err = drm_dp_dpcd_read_link_status(aux, status);
	if (err < 0) {
		DRM_ERROR("failed to read link status: %d\n", err);
		return err;
	}

	if (!drm_dp_clock_recovery_ok(status, link->lanes)) {
		DRM_ERROR("clock recovery lost while equalizing channel\n");
		link->train.clock_recovered = false;
		drm_dp_link_get_adjustments(link, status);
		return 0;
	}

	if (!drm_dp_channel_eq_ok(status, link->lanes))
		drm_dp_link_get_adjustments(link, status);
	else
		link->train.channel_equalized = true;

	return 0;
}

static int drm_dp_link_channel_equalization(struct drm_dp_link *link)
{
	unsigned int repeat;
	int err;

	/* start channel equalization using pattern 2 or 3 */
	if (link->capabilities & DP_LINK_CAP_TPS3)
		link->train.pattern = DP_TRAINING_PATTERN_3;
	else
		link->train.pattern = DP_TRAINING_PATTERN_2;

	for (repeat = 1; repeat < 8; repeat++) {
		err = drm_dp_link_equalize_channel(link);
		if (err < 0) {
			DRM_ERROR("failed to equalize channel: %d\n", err);
			return err;
		}

		drm_dp_link_train_adjust(&link->train);

		if (link->train.channel_equalized)
			break;
	}

	return 0;
}

static int drm_dp_link_downgrade(struct drm_dp_link *link)
{
	switch (link->rate) {
	case 162000:
		return -EINVAL;

	case 270000:
		link->rate = 162000;
		break;

	case 540000:
		link->rate = 270000;
		return 0;
	}

	return 0;
}

static void drm_dp_link_train_disable(struct drm_dp_link *link)
{
	int err;

	link->train.pattern = DP_TRAINING_PATTERN_DISABLE;

	err = drm_dp_link_apply_training(link);
	if (err < 0)
		DRM_ERROR("failed to disable link training: %d\n", err);
}

static int drm_dp_link_train_full(struct drm_dp_link *link)
{
	int err;

retry:
	DRM_DEBUG_KMS("full-training link: %u lane%s at %u MHz\n",
		      link->lanes, (link->lanes > 1) ? "s" : "",
		      link->rate / 100);

	err = drm_dp_link_configure(link->aux, link);
	if (err < 0) {
		DRM_ERROR("failed to configure DP link: %d\n", err);
		return err;
	}

	err = drm_dp_link_clock_recovery(link);
	if (err < 0) {
		DRM_ERROR("clock recovery failed: %d\n", err);
		goto out;
	}

	if (!link->train.clock_recovered) {
		DRM_ERROR("clock recovery failed, downgrading link\n");

		err = drm_dp_link_downgrade(link);
		if (err < 0)
			goto out;

		goto retry;
	}

	DRM_DEBUG_KMS("clock recovery succeeded\n");

	err = drm_dp_link_channel_equalization(link);
	if (err < 0) {
		DRM_ERROR("channel equalization failed: %d\n", err);
		goto out;
	}

	if (!link->train.channel_equalized) {
		DRM_ERROR("channel equalization failed, downgrading link\n");

		err = drm_dp_link_downgrade(link);
		if (err < 0)
			goto out;

		goto retry;
	}

	DRM_DEBUG_KMS("channel equalization succeeded\n");

out:
	drm_dp_link_train_disable(link);
	return err;
}

static int drm_dp_link_train_fast(struct drm_dp_link *link)
{
	u8 status[DP_LINK_STATUS_SIZE];
	int err;

	DRM_DEBUG_KMS("fast-training link: %u lane%s at %u MHz\n",
		      link->lanes, (link->lanes > 1) ? "s" : "",
		      link->rate / 100);

	err = drm_dp_link_configure(link->aux, link);
	if (err < 0) {
		DRM_ERROR("failed to configure DP link: %d\n", err);
		return err;
	}

	/* transmit training pattern 1 for 500 microseconds */
	link->train.pattern = DP_TRAINING_PATTERN_1;

	err = drm_dp_link_apply_training(link);
	if (err < 0)
		goto out;

	usleep_range(500, 1000);

	/* transmit training pattern 2 or 3 for 500 microseconds */
	if (link->capabilities & DP_LINK_CAP_TPS3)
		link->train.pattern = DP_TRAINING_PATTERN_3;
	else
		link->train.pattern = DP_TRAINING_PATTERN_2;

	err = drm_dp_link_apply_training(link);
	if (err < 0)
		goto out;

	usleep_range(500, 1000);

	err = drm_dp_dpcd_read_link_status(link->aux, status);
	if (err < 0) {
		DRM_ERROR("failed to read link status: %d\n", err);
		goto out;
	}

	if (!drm_dp_clock_recovery_ok(status, link->lanes)) {
		DRM_ERROR("clock recovery failed\n");
		err = -EIO;
	}

	if (!drm_dp_channel_eq_ok(status, link->lanes)) {
		DRM_ERROR("channel equalization failed\n");
		err = -EIO;
	}

out:
	drm_dp_link_train_disable(link);
	return err;
}

/**
 * drm_dp_link_train() - perform DisplayPort link training
 * @link: a DP link object
 *
 * Uses the context stored in the DP link object to perform link training. It
 * is expected that drivers will call drm_dp_link_probe() to obtain the link
 * capabilities before performing link training.
 *
 * If the sink supports fast link training (no AUX CH handshake) and valid
 * training settings are available, this function will try to perform fast
 * link training and fall back to full link training on failure.
 *
 * Returns: 0 on success or a negative error code on failure.
 */
int drm_dp_link_train(struct drm_dp_link *link)
{
	int err;

	if (link->capabilities & DP_LINK_CAP_FAST_TRAINING) {
		if (drm_dp_link_train_valid(&link->train)) {
			err = drm_dp_link_train_fast(link);
			if (err < 0)
				DRM_ERROR("fast link training failed: %d\n",
					  err);
			else
				return 0;
		} else {
			DRM_DEBUG_KMS("training parameters not available\n");
		}
	} else {
		DRM_DEBUG_KMS("fast link training not supported\n");
	}

	err = drm_dp_link_train_full(link);
	if (err < 0)
		DRM_ERROR("full link training failed: %d\n", err);

	return err;
}
EXPORT_SYMBOL(drm_dp_link_train);
