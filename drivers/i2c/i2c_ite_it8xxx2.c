/*
 * Copyright (c) 2020 ITE Corporation. All Rights Reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ite_it8xxx2_i2c

#include <drivers/gpio.h>
#include <drivers/i2c.h>
#include <drivers/pinmux.h>
#include <errno.h>
#include <logging/log.h>
		LOG_MODULE_REGISTER(i2c_ite_it8xxx2);
#include "i2c-priv.h"
#include <soc.h>
#include <soc_dt.h>
#include <sys/util.h>

#define I2C_STANDARD_PORT_COUNT 3
/* Default PLL frequency. */
#define PLL_CLOCK 48000000

/*
 * Structure i2c_alts_cfg is about the alternate function
 * setting of i2c, this config will be used at initial
 * time and recover bus.
 */
struct i2c_alts_cfg {
	/* Pinmux control group */
	const struct device *pinctrls;
	/* GPIO pin */
	uint8_t pin;
	/* Alternate function */
	uint8_t alt_fun;
};

struct i2c_it8xxx2_config {
	void (*irq_config_func)(void);
	uint32_t bitrate;
	uint8_t *base;
	uint8_t i2c_irq_base;
	uint8_t port;
	/* I2C alternate configuration */
	const struct i2c_alts_cfg *alts_list;
	/* GPIO handle */
	const struct device *gpio_dev;
};

enum i2c_pin_fun {
	SCL = 0,
	SDA,
};

enum i2c_ch_status {
	I2C_CH_NORMAL = 0,
	I2C_CH_REPEAT_START,
	I2C_CH_WAIT_READ,
	I2C_CH_WAIT_NEXT_XFER,
};

struct i2c_it8xxx2_data {
	enum i2c_ch_status i2ccs;
	struct i2c_msg *msgs;
	struct k_mutex mutex;
	struct k_sem device_sync_sem;
	/* Index into output data */
	size_t widx;
	/* Index into input data */
	size_t ridx;
	/* operation freq of i2c */
	uint32_t bus_freq;
	/* Error code, if any */
	uint32_t err;
	/* address of device */
	uint16_t addr_16bit;
	/* Frequency setting */
	uint8_t freq;
	/* wait for stop bit interrupt */
	uint8_t stop;
};

enum enhanced_i2c_transfer_direct {
	TX_DIRECT,
	RX_DIRECT,
};

enum enhanced_i2c_ctl {
	/* Hardware reset */
	E_HW_RST = 0x01,
	/* Stop */
	E_STOP = 0x02,
	/* Start & Repeat start */
	E_START = 0x04,
	/* Acknowledge */
	E_ACK = 0x08,
	/* State reset */
	E_STS_RST = 0x10,
	/* Mode select */
	E_MODE_SEL = 0x20,
	/* I2C interrupt enable */
	E_INT_EN = 0x40,
	/* 0 : Standard mode , 1 : Receive mode */
	E_RX_MODE = 0x80,
	/* State reset and hardware reset */
	E_STS_AND_HW_RST = (E_STS_RST | E_HW_RST),
	/* Generate start condition and transmit slave address */
	E_START_ID = (E_INT_EN | E_MODE_SEL | E_ACK | E_START | E_HW_RST),
	/* Generate stop condition */
	E_FINISH = (E_INT_EN | E_MODE_SEL | E_ACK | E_STOP | E_HW_RST),
};

enum enhanced_i2c_host_status {
	/* ACK receive */
	E_HOSTA_ACK = 0x01,
	/* Interrupt pending */
	E_HOSTA_INTP = 0x02,
	/* Read/Write */
	E_HOSTA_RW = 0x04,
	/* Time out error */
	E_HOSTA_TMOE = 0x08,
	/* Arbitration lost */
	E_HOSTA_ARB = 0x10,
	/* Bus busy */
	E_HOSTA_BB = 0x20,
	/* Address match */
	E_HOSTA_AM = 0x40,
	/* Byte done status */
	E_HOSTA_BDS = 0x80,
	/* time out or lost arbitration */
	E_HOSTA_ANY_ERROR = (E_HOSTA_TMOE | E_HOSTA_ARB),
	/* Byte transfer done and ACK receive */
	E_HOSTA_BDS_AND_ACK = (E_HOSTA_BDS | E_HOSTA_ACK),
};

enum i2c_reset_cause {
	I2C_RC_NO_IDLE_FOR_START = 1,
	I2C_RC_TIMEOUT,
};

/* Start smbus session from idle state */
#define I2C_MSG_START BIT(5)

#define I2C_LINE_SCL_HIGH BIT(0)
#define I2C_LINE_SDA_HIGH BIT(1)
#define I2C_LINE_IDLE (I2C_LINE_SCL_HIGH | I2C_LINE_SDA_HIGH)

static int i2c_parsing_return_value(const struct device *dev)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;

	if (!data->err)
		return 0;

	/* Connection timed out */
	if (data->err == ETIMEDOUT)
		return -ETIMEDOUT;

	if (config->port < I2C_STANDARD_PORT_COUNT) {
		/* The device does not respond ACK */
		if (data->err == HOSTA_NACK)
			return -ENXIO;
		else
			return -EIO;
	} else {
		/* The device does not respond ACK */
		if (data->err == E_HOSTA_ACK)
			return -ENXIO;
		else
			return -EIO;
	}
}

static int i2c_get_line_levels(const struct device *dev)
{
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;
	int pin_sts = 0;

	if (config->port < I2C_STANDARD_PORT_COUNT) {
		return IT83XX_SMB_SMBPCTL(base) & 0x03;
	}

	if (IT83XX_I2C_TOS(base) & IT8XXX2_I2C_SCL_IN) {
		pin_sts |= I2C_LINE_SCL_HIGH;
	}

	if (IT83XX_I2C_TOS(base) & IT8XXX2_I2C_SDA_IN) {
		pin_sts |= I2C_LINE_SDA_HIGH;
	}

	return pin_sts;
}

static int i2c_is_busy(const struct device *dev)
{
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;

	if (config->port < I2C_STANDARD_PORT_COUNT) {
		return (IT83XX_SMB_HOSTA(base) &
					(HOSTA_HOBY | HOSTA_ALL_WC_BIT));
	}

	return (IT83XX_I2C_STR(base) & E_HOSTA_BB);
}

static int i2c_bus_not_available(const struct device *dev)
{
	if (i2c_is_busy(dev) ||
		(i2c_get_line_levels(dev) != I2C_LINE_IDLE)) {
		return -EIO;
	}

	return 0;
}

static void i2c_reset(const struct device *dev)
{
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;

	if (config->port < I2C_STANDARD_PORT_COUNT) {
		/* bit1, kill current transaction. */
		IT83XX_SMB_HOCTL(base) = 0x2;
		IT83XX_SMB_HOCTL(base) = 0;
		/* W/C host status register */
		IT83XX_SMB_HOSTA(base) = HOSTA_ALL_WC_BIT;
	} else {
		/* State reset and hardware reset */
		IT83XX_I2C_CTR(base) = E_STS_AND_HW_RST;
	}
}

/*
 * Set i2c standard port (A, B, or C) runs at 400kHz by using timing registers
 * (offset 0h ~ 7h).
 */
static void i2c_standard_port_timing_regs_400khz(uint8_t port)
{
	/* Port clock frequency depends on setting of timing registers. */
	IT83XX_SMB_SCLKTS(port) = 0;
	/* Suggested setting of timing registers of 400kHz. */
	IT83XX_SMB_4P7USL = 0x6;
	IT83XX_SMB_4P0USL = 0;
	IT83XX_SMB_300NS = 0x1;
	IT83XX_SMB_250NS = 0x2;
	IT83XX_SMB_45P3USL = 0x6a;
	IT83XX_SMB_45P3USH = 0x1;
	IT83XX_SMB_4P7A4P0H = 0;
}

/* Set clock frequency for i2c port A, B , or C */
static void i2c_standard_port_set_frequency(const struct device *dev,
					int freq_khz, int freq_set)
{
	const struct i2c_it8xxx2_config *config = dev->config;

	/*
	 * If port's clock frequency is 400kHz, we use timing registers
	 * for setting. So we can adjust tlow to meet timing.
	 * The others use basic 50/100/1000 KHz setting.
	 */
	if (freq_khz == 400) {
		i2c_standard_port_timing_regs_400khz(config->port);
	} else {
		IT83XX_SMB_SCLKTS(config->port) = freq_set;
	}

	/* This field defines the SMCLK0/1/2 clock/data low timeout. */
	IT83XX_SMB_25MS = I2C_CLK_LOW_TIMEOUT;
}

/* Set clock frequency for i2c port D, E , or F */
static void i2c_enhanced_port_set_frequency(const struct device *dev,
				int freq_khz)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;
	uint32_t clk_div, psr;
	uint8_t *base = config->base;

	/*
	 * Let psr(Prescale) = IT83XX_I2C_PSR(p_ch)
	 * Then, 1 SCL cycle = 2 x (psr + 2) x SMBus clock cycle
	 * SMBus clock = PLL_CLOCK / clk_div
	 * SMBus clock cycle = 1 / SMBus clock
	 * 1 SCL cycle = 1 / (1000 x freq)
	 * 1 / (1000 x freq) =
	 *			2 x (psr + 2) x (1 / (PLL_CLOCK / clk_div))
	 * psr = ((PLL_CLOCK / clk_div) x
	 *			(1 / (1000 x freq)) x (1 / 2)) - 2
	 */
	if (freq_khz) {
		/* Get SMBus clock divide value */
		clk_div = (SCDCR2 & 0x0F) + 1U;
		/* Calculate PSR value */
		psr = (PLL_CLOCK / (clk_div * (2U * 1000U * freq_khz))) - 2U;
		/* Set psr value under 0xFD */
		if (psr > 0xFD) {
			psr = 0xFD;
		}

		/* Set I2C Speed */
		IT83XX_I2C_PSR(base) = psr & 0xFF;
		IT83XX_I2C_HSPR(base) = psr & 0xFF;
		/* Backup */
		data->freq = psr & 0xFF;
	}

}

static int i2c_it8xxx2_configure(const struct device *dev,
				uint32_t dev_config_raw)
{
	const struct i2c_it8xxx2_config *config = dev->config;
	struct i2c_it8xxx2_data *const data = dev->data;
	uint32_t freq, freq_set;

	if (!(I2C_MODE_MASTER & dev_config_raw)) {
		return -EINVAL;
	}

	if (I2C_ADDR_10_BITS & dev_config_raw) {
		return -EINVAL;
	}

	data->bus_freq = I2C_SPEED_GET(dev_config_raw);

	switch (data->bus_freq) {
	case I2C_SPEED_STANDARD:
		freq = 100;
		freq_set = 2;
		break;
	case I2C_SPEED_FAST:
		freq = 400;
		freq_set = 3;
		break;
	case I2C_SPEED_FAST_PLUS:
		freq = 1000;
		freq_set = 4;
		break;
	default:
		return -EINVAL;
	}

	if (config->port < I2C_STANDARD_PORT_COUNT) {
		i2c_standard_port_set_frequency(dev, freq, freq_set);
	} else {
		i2c_enhanced_port_set_frequency(dev, freq);
	}

	return 0;
}

static int i2c_it8xxx2_get_config(const struct device *dev,
				  uint32_t *dev_config)
{
	struct i2c_it8xxx2_data *const data = dev->data;
	uint32_t speed;

	if (!data->bus_freq) {
		LOG_ERR("The bus frequency is not initially configured.");
		return -EIO;
	}

	switch (data->bus_freq) {
	case I2C_SPEED_STANDARD:
		speed = I2C_SPEED_SET(I2C_SPEED_STANDARD);
		break;
	case I2C_SPEED_FAST:
		speed = I2C_SPEED_SET(I2C_SPEED_FAST);
		break;
	case I2C_SPEED_FAST_PLUS:
		speed = I2C_SPEED_SET(I2C_SPEED_FAST_PLUS);
		break;
	default:
		return -ERANGE;
	}

	*dev_config = (I2C_MODE_MASTER | speed);

	return 0;
}

static int enhanced_i2c_error(const struct device *dev)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;
	uint32_t i2c_str = IT83XX_I2C_STR(base);

	if (i2c_str & E_HOSTA_ANY_ERROR) {
		data->err = i2c_str & E_HOSTA_ANY_ERROR;
	/* device does not respond ACK */
	} else if ((i2c_str & E_HOSTA_BDS_AND_ACK) == E_HOSTA_BDS) {
		if (IT83XX_I2C_CTR(base) & E_ACK)
			data->err = E_HOSTA_ACK;
	}

	return data->err;
}

static void enhanced_i2c_start(const struct device *dev)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;

	/* State reset and hardware reset */
	IT83XX_I2C_CTR(base) = E_STS_AND_HW_RST;
	/* Set i2c frequency */
	IT83XX_I2C_PSR(base) = data->freq;
	IT83XX_I2C_HSPR(base) = data->freq;
	/*
	 * Set time out register.
	 * I2C D/E/F clock/data low timeout.
	 */
	IT83XX_I2C_TOR(base) = I2C_CLK_LOW_TIMEOUT;
	/* bit1: Enable enhanced i2c module */
	IT83XX_I2C_CTR1(base) = BIT(1);
}

static void i2c_pio_trans_data(const struct device *dev,
				enum enhanced_i2c_transfer_direct direct,
				uint16_t trans_data, int first_byte)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;
	uint32_t nack = 0;

	if (first_byte) {
		/* First byte must be slave address. */
		IT83XX_I2C_DTR(base) = trans_data |
					(direct == RX_DIRECT ? BIT(0) : 0);
		/* start or repeat start signal. */
		IT83XX_I2C_CTR(base) = E_START_ID;
	} else {
		if (direct == TX_DIRECT) {
			/* Transmit data */
			IT83XX_I2C_DTR(base) = (uint8_t)trans_data;
		} else {
			/*
			 * Receive data.
			 * Last byte should be NACK in the end of read cycle
			 */
			if (((data->ridx + 1) == data->msgs->len) &&
				(data->msgs->flags & I2C_MSG_STOP)) {
				nack = 1;
			}
		}
		/* Set hardware reset to start next transmission */
		IT83XX_I2C_CTR(base) = E_INT_EN | E_MODE_SEL |
				E_HW_RST | (nack ? 0 : E_ACK);
	}
}

static int enhanced_i2c_tran_read(const struct device *dev)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;
	uint8_t in_data = 0;

	if (data->msgs->flags & I2C_MSG_START) {
		/* clear start flag */
		data->msgs->flags &= ~I2C_MSG_START;
		enhanced_i2c_start(dev);
		/* Direct read  */
		data->i2ccs = I2C_CH_WAIT_READ;
		/* Send ID */
		i2c_pio_trans_data(dev, RX_DIRECT, data->addr_16bit << 1, 1);
	} else {
		if (data->i2ccs) {
			if (data->i2ccs == I2C_CH_WAIT_READ) {
				data->i2ccs = I2C_CH_NORMAL;
				/* Receive data */
				i2c_pio_trans_data(dev, RX_DIRECT, in_data, 0);

			/* data->msgs->flags == I2C_MSG_RESTART */
			} else {
				/* Write to read */
				data->i2ccs = I2C_CH_WAIT_READ;
				/* Send ID */
				i2c_pio_trans_data(dev, RX_DIRECT,
					data->addr_16bit << 1, 1);
			}
			/* Turn on irq before next direct read */
			irq_enable(config->i2c_irq_base);
		} else {
			if (data->ridx < data->msgs->len) {
				/* read data */
				*(data->msgs->buf++) = IT83XX_I2C_DRR(base);
				data->ridx++;
				/* done */
				if (data->ridx == data->msgs->len) {
					data->msgs->len = 0;
					if (data->msgs->flags & I2C_MSG_STOP) {
						data->i2ccs = I2C_CH_NORMAL;
						IT83XX_I2C_CTR(base) = E_FINISH;
						/* wait for stop bit interrupt */
						data->stop = 1;
						return 1;
					}
					/* End the transaction */
					data->i2ccs = I2C_CH_WAIT_READ;
					return 0;
				}
				/* read next byte */
				i2c_pio_trans_data(dev, RX_DIRECT, in_data, 0);
			}
		}
	}
	return 1;
}

static int enhanced_i2c_tran_write(const struct device *dev)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;
	uint8_t out_data;

	if (data->msgs->flags & I2C_MSG_START) {
		/* Clear start bit */
		data->msgs->flags &= ~I2C_MSG_START;
		enhanced_i2c_start(dev);
		/* Send ID */
		i2c_pio_trans_data(dev, TX_DIRECT, data->addr_16bit << 1, 1);
	} else {
		/* Host has completed the transmission of a byte */
		if (data->widx < data->msgs->len) {
			out_data = *(data->msgs->buf++);
			data->widx++;

			/* Send Byte */
			i2c_pio_trans_data(dev, TX_DIRECT, out_data, 0);
			if (data->i2ccs == I2C_CH_WAIT_NEXT_XFER) {
				data->i2ccs = I2C_CH_NORMAL;
				irq_enable(config->i2c_irq_base);
			}
		} else {
			/* done */
			data->msgs->len = 0;
			if (data->msgs->flags & I2C_MSG_STOP) {
				IT83XX_I2C_CTR(base) = E_FINISH;
				/* wait for stop bit interrupt */
				data->stop = 1;
			} else {
				/* Direct write with direct read */
				data->i2ccs = I2C_CH_WAIT_NEXT_XFER;
				return 0;
			}
		}
	}
	return 1;
}

static void i2c_r_last_byte(const struct device *dev)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;

	/*
	 * bit5, The firmware shall write 1 to this bit
	 * when the next byte will be the last byte for i2c read.
	 */
	if ((data->msgs->flags & I2C_MSG_STOP) &&
					(data->ridx == data->msgs->len - 1)) {
		IT83XX_SMB_HOCTL(base) |= 0x20;
	}
}

static void i2c_w2r_change_direction(const struct device *dev)
{
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;

	/* I2C switch direction */
	if (IT83XX_SMB_HOCTL2(base) & 0x08) {
		i2c_r_last_byte(dev);
		IT83XX_SMB_HOSTA(base) = HOSTA_NEXT_BYTE;
	} else {
		/*
		 * bit2, I2C switch direction wait.
		 * bit3, I2C switch direction enable.
		 */
		IT83XX_SMB_HOCTL2(base) |= 0x0C;
		IT83XX_SMB_HOSTA(base) = HOSTA_NEXT_BYTE;
		i2c_r_last_byte(dev);
		IT83XX_SMB_HOCTL2(base) &= ~0x04;
	}
}

static int i2c_tran_read(const struct device *dev)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;

	if (data->msgs->flags & I2C_MSG_START) {
		/* i2c enable */
		IT83XX_SMB_HOCTL2(base) = 0x13;
		/*
		 * bit0, Direction of the host transfer.
		 * bit[1:7}, Address of the targeted slave.
		 */
		IT83XX_SMB_TRASLA(base) = (uint8_t)(data->addr_16bit << 1) | 0x01;
		/* clear start flag */
		data->msgs->flags &= ~I2C_MSG_START;
		/*
		 * bit0, Host interrupt enable.
		 * bit[2:4}, Extend command.
		 * bit5, The firmware shall write 1 to this bit
		 *       when the next byte will be the last byte.
		 * bit6, start.
		 */
		if ((data->msgs->len == 1) &&
					(data->msgs->flags & I2C_MSG_STOP)) {
			IT83XX_SMB_HOCTL(base) = 0x7D;
		} else {
			IT83XX_SMB_HOCTL(base) = 0x5D;
		}
	} else {
		if ((data->i2ccs == I2C_CH_REPEAT_START) ||
			(data->i2ccs == I2C_CH_WAIT_READ)) {
			if (data->i2ccs == I2C_CH_REPEAT_START) {
				/* write to read */
				i2c_w2r_change_direction(dev);
			} else {
				/* For last byte */
				i2c_r_last_byte(dev);
				/* W/C for next byte */
				IT83XX_SMB_HOSTA(base) = HOSTA_NEXT_BYTE;
			}
			data->i2ccs = I2C_CH_NORMAL;
			irq_enable(config->i2c_irq_base);
		} else if (IT83XX_SMB_HOSTA(base) & HOSTA_BDS) {
			if (data->ridx < data->msgs->len) {
				/* To get received data. */
				*(data->msgs->buf++) = IT83XX_SMB_HOBDB(base);
				data->ridx++;
				/* For last byte */
				i2c_r_last_byte(dev);
				/* done */
				if (data->ridx == data->msgs->len) {
					data->msgs->len = 0;
					if (data->msgs->flags & I2C_MSG_STOP) {
						/* W/C for finish */
						IT83XX_SMB_HOSTA(base) =
						HOSTA_NEXT_BYTE;

						data->stop = 1;
					} else {
						data->i2ccs = I2C_CH_WAIT_READ;
						return 0;
					}
				} else {
					/* W/C for next byte */
					IT83XX_SMB_HOSTA(base) =
							HOSTA_NEXT_BYTE;
				}
			}
		}
	}
	return 1;

}

static int i2c_tran_write(const struct device *dev)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;

	if (data->msgs->flags & I2C_MSG_START) {
		/* i2c enable */
		IT83XX_SMB_HOCTL2(base) = 0x13;
		/*
		 * bit0, Direction of the host transfer.
		 * bit[1:7}, Address of the targeted slave.
		 */
		IT83XX_SMB_TRASLA(base) = (uint8_t)data->addr_16bit << 1;
		/* Send first byte */
		IT83XX_SMB_HOBDB(base) = *(data->msgs->buf++);

		data->widx++;
		/* clear start flag */
		data->msgs->flags &= ~I2C_MSG_START;
		/*
		 * bit0, Host interrupt enable.
		 * bit[2:4}, Extend command.
		 * bit6, start.
		 */
		IT83XX_SMB_HOCTL(base) = 0x5D;
	} else {
		/* Host has completed the transmission of a byte */
		if (IT83XX_SMB_HOSTA(base) & HOSTA_BDS) {
			if (data->widx < data->msgs->len) {
				/* Send next byte */
				IT83XX_SMB_HOBDB(base) = *(data->msgs->buf++);

				data->widx++;
				/* W/C byte done for next byte */
				IT83XX_SMB_HOSTA(base) = HOSTA_NEXT_BYTE;

				if (data->i2ccs == I2C_CH_REPEAT_START) {
					data->i2ccs = I2C_CH_NORMAL;
					irq_enable(config->i2c_irq_base);
				}
			} else {
				/* done */
				data->msgs->len = 0;
				if (data->msgs->flags & I2C_MSG_STOP) {
					/* set I2C_EN = 0 */
					IT83XX_SMB_HOCTL2(base) = 0x11;
					/* W/C byte done for finish */
					IT83XX_SMB_HOSTA(base) =
							HOSTA_NEXT_BYTE;

					data->stop = 1;
				} else {
					data->i2ccs = I2C_CH_REPEAT_START;
					return 0;
				}
			}
		}
	}
	return 1;

}

static int i2c_transaction(const struct device *dev)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;

	if (config->port < I2C_STANDARD_PORT_COUNT) {
		/* any error */
		if (IT83XX_SMB_HOSTA(base) & HOSTA_ANY_ERROR) {
			data->err = (IT83XX_SMB_HOSTA(base) & HOSTA_ANY_ERROR);
		} else {
			if (!data->stop) {
				if (data->msgs->flags & I2C_MSG_READ) {
					return i2c_tran_read(dev);
				} else {
					return i2c_tran_write(dev);
				}
			}
			/* wait finish */
			if (!(IT83XX_SMB_HOSTA(base) & HOSTA_FINTR)) {
				return 1;
			}
		}
		/* W/C */
		IT83XX_SMB_HOSTA(base) = HOSTA_ALL_WC_BIT;
		/* disable the SMBus host interface */
		IT83XX_SMB_HOCTL2(base) = 0x00;
	} else {

		/* no error */
		if (!(enhanced_i2c_error(dev))) {
			if (!data->stop) {
				/* i2c read */
				if (data->msgs->flags & I2C_MSG_READ) {
					return enhanced_i2c_tran_read(dev);
				/* i2c write */
				} else {
					return enhanced_i2c_tran_write(dev);
				}
			}
		}
		IT83XX_I2C_CTR(base) = E_STS_AND_HW_RST;
		IT83XX_I2C_CTR1(base) = 0;
	}
	data->stop = 0;
	/* done doing work */
	return 0;
}

static int i2c_it8xxx2_transfer(const struct device *dev, struct i2c_msg *msgs,
		uint8_t num_msgs, uint16_t addr)
{
	struct i2c_it8xxx2_data *data;
	const struct i2c_it8xxx2_config *config;
	int res;

	/* Check for NULL pointers */
	if (dev == NULL) {
		LOG_ERR("Device handle is NULL");
		return -EINVAL;
	}
	if (msgs == NULL) {
		LOG_ERR("Device message is NULL");
		return -EINVAL;
	}

	data = dev->data;
	config = dev->config;

	/* Lock mutex of i2c controller */
	k_mutex_lock(&data->mutex, K_FOREVER);
	/*
	 * If the transaction of write to read is divided into two
	 * transfers, the repeat start transfer uses this flag to
	 * exclude checking bus busy.
	 */
	if (data->i2ccs == I2C_CH_NORMAL) {
		/* Make sure we're in a good state to start */
		if (i2c_bus_not_available(dev)) {
			/* Recovery I2C bus */
			i2c_recover_bus(dev);
			/*
			 * After resetting I2C bus, if I2C bus is not available
			 * (No external pull-up), drop the transaction.
			 */
			if (i2c_bus_not_available(dev)) {
				/* Unlock mutex of i2c controller */
				k_mutex_unlock(&data->mutex);
				return -EIO;
			}
		}

		msgs->flags |= I2C_MSG_START;
	}

	for (int i = 0; i < num_msgs; i++) {

		data->widx = 0;
		data->ridx = 0;
		data->err = 0;
		data->msgs = &(msgs[i]);
		data->addr_16bit = addr;

		if (msgs->flags & I2C_MSG_START) {
			data->i2ccs = I2C_CH_NORMAL;
			/* enable i2c interrupt */
			irq_enable(config->i2c_irq_base);
		}
		/* Start transaction */
		i2c_transaction(dev);
		/* Wait for the transfer to complete */
		/* TODO: the timeout should be adjustable */
		res = k_sem_take(&data->device_sync_sem, K_MSEC(100));
		/*
		 * The irq will be enabled at the condition of start or
		 * repeat start of I2C. If timeout occurs without being
		 * wake up during suspend(ex: interrupt is not fired),
		 * the irq should be disabled immediately.
		 */
		irq_disable(config->i2c_irq_base);
		/*
		 * The transaction is dropped on any error(timeout, NACK, fail,
		 * bus error, device error).
		 */
		if (data->err)
			break;

		if (res != 0) {
			data->err = ETIMEDOUT;
			/* reset i2c port */
			i2c_reset(dev);
			printk("I2C ch%d:0x%X reset cause %d\n",
			       config->port, data->addr_16bit, I2C_RC_TIMEOUT);
			/* If this message is sent fail, drop the transaction. */
			break;
		}
	}

	/* reset i2c channel status */
	if (data->err || (msgs->flags & I2C_MSG_STOP)) {
		data->i2ccs = I2C_CH_NORMAL;
	}
	/* Unlock mutex of i2c controller */
	k_mutex_unlock(&data->mutex);

	return i2c_parsing_return_value(dev);
}

static void i2c_it8xxx2_isr(const struct device *dev)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;

	/* If done doing work, wake up the task waiting for the transfer */
	if (!i2c_transaction(dev)) {
		k_sem_give(&data->device_sync_sem);
		irq_disable(config->i2c_irq_base);
	}
}

static int i2c_it8xxx2_init(const struct device *dev)
{
	struct i2c_it8xxx2_data *data = dev->data;
	const struct i2c_it8xxx2_config *config = dev->config;
	uint8_t *base = config->base;
	uint32_t bitrate_cfg, offset = 0;
	int error;

	/*
	 * This register is a pre-define hardware slave A and can
	 * be accessed through I2C0. It is not currently used, so
	 * it can be disabled to avoid illegal access.
	 */
	IT8XXX2_SMB_SFFCTL &= ~IT8XXX2_SMB_HSAPE;

	/* Initialize mutex and semaphore */
	k_mutex_init(&data->mutex);
	k_sem_init(&data->device_sync_sem, 0, K_SEM_MAX_LIMIT);

	switch ((uint32_t)base) {
	case DT_REG_ADDR(DT_NODELABEL(i2c0)):
		offset = CGC_OFFSET_SMBA;
		break;
	case DT_REG_ADDR(DT_NODELABEL(i2c1)):
		offset = CGC_OFFSET_SMBB;
		break;
	case DT_REG_ADDR(DT_NODELABEL(i2c2)):
		offset = CGC_OFFSET_SMBC;
		break;
	case DT_REG_ADDR(DT_NODELABEL(i2c3)):
		offset = CGC_OFFSET_SMBD;
		break;
	case DT_REG_ADDR(DT_NODELABEL(i2c4)):
		offset = CGC_OFFSET_SMBE;
		/* Enable SMBus E channel */
		PMER1 |= 0x01;
		break;
	case DT_REG_ADDR(DT_NODELABEL(i2c5)):
		offset = CGC_OFFSET_SMBF;
		/* Enable SMBus F channel */
		PMER1 |= 0x02;
		break;
	}

	/* Enable I2C function. */
	volatile uint8_t *reg = (volatile uint8_t *)
				(IT83XX_ECPM_BASE + (offset >> 8));
	uint8_t reg_mask = offset & 0xff;
	*reg &= ~reg_mask;

	if (config->port < I2C_STANDARD_PORT_COUNT) {
		/*
		 * bit0, The SMBus host interface is enabled.
		 * bit1, Enable to communicate with I2C device
		 *		  and support I2C-compatible cycles.
		 * bit4, This bit controls the reset mechanism
		 *		  of SMBus master to handle the SMDAT
		 *		  line low if 25ms reg timeout.
		 */
		IT83XX_SMB_HOCTL2(base) = 0x11;
		/*
		 * bit1, Kill SMBus host transaction.
		 * bit0, Enable the interrupt for the master interface.
		 */
		IT83XX_SMB_HOCTL(base) = 0x03;
		IT83XX_SMB_HOCTL(base) = 0x01;

		/* W/C host status register */
		IT83XX_SMB_HOSTA(base) = HOSTA_ALL_WC_BIT;
		IT83XX_SMB_HOCTL2(base) = 0x00;

	} else {
		/* Software reset */
		IT83XX_I2C_DHTR(base) |= 0x80;
		IT83XX_I2C_DHTR(base) &= 0x7F;
		/* State reset and hardware reset */
		IT83XX_I2C_CTR(base) = E_STS_AND_HW_RST;
		/* bit1, Module enable */
		IT83XX_I2C_CTR1(base) = 0;
	}

	bitrate_cfg = i2c_map_dt_bitrate(config->bitrate);
	error = i2c_it8xxx2_configure(dev, I2C_MODE_MASTER | bitrate_cfg);
	data->i2ccs = I2C_CH_NORMAL;

	if (error) {
		LOG_ERR("i2c: failure initializing");
		return error;
	}

	/* The pin is set to I2C alternate function of SCL */
	pinmux_pin_set(config->alts_list[SCL].pinctrls,
		       config->alts_list[SCL].pin,
		       config->alts_list[SCL].alt_fun);
	/* The pin is set to I2C alternate function of SDA */
	pinmux_pin_set(config->alts_list[SDA].pinctrls,
		       config->alts_list[SDA].pin,
		       config->alts_list[SDA].alt_fun);

	return 0;
}

static int i2c_it8xxx2_recover_bus(const struct device *dev)
{
	const struct i2c_it8xxx2_config *config = dev->config;
	int i;

	/* Set SCL of I2C as GPIO pin */
	pinmux_pin_input_enable(config->alts_list[SCL].pinctrls,
				config->alts_list[SCL].pin,
				PINMUX_OUTPUT_ENABLED);
	/* Set SDA of I2C as GPIO pin */
	pinmux_pin_input_enable(config->alts_list[SDA].pinctrls,
				config->alts_list[SDA].pin,
				PINMUX_OUTPUT_ENABLED);

	/* Pull SCL and SDA pin to high */
	gpio_pin_set(config->gpio_dev, config->alts_list[SCL].pin, 1);
	gpio_pin_set(config->gpio_dev, config->alts_list[SDA].pin, 1);
	k_msleep(1);

	/* Start condition */
	gpio_pin_set(config->gpio_dev, config->alts_list[SDA].pin, 0);
	k_msleep(1);
	gpio_pin_set(config->gpio_dev, config->alts_list[SCL].pin, 0);
	k_msleep(1);

	/* 9 cycles of SCL with SDA held high */
	for (i = 0; i < 9; i++) {
		/* SDA */
		gpio_pin_set(config->gpio_dev, config->alts_list[SDA].pin, 1);
		/* SCL */
		gpio_pin_set(config->gpio_dev, config->alts_list[SCL].pin, 1);
		k_msleep(1);
		/* SCL */
		gpio_pin_set(config->gpio_dev, config->alts_list[SCL].pin, 0);
		k_msleep(1);
	}
	/* SDA */
	gpio_pin_set(config->gpio_dev, config->alts_list[SDA].pin, 0);
	k_msleep(1);

	/* Stop condition */
	gpio_pin_set(config->gpio_dev, config->alts_list[SCL].pin, 1);
	k_msleep(1);
	gpio_pin_set(config->gpio_dev, config->alts_list[SDA].pin, 1);
	k_msleep(1);

	/* Set GPIO back to I2C alternate function of SCL */
	pinmux_pin_set(config->alts_list[SCL].pinctrls,
		       config->alts_list[SCL].pin,
		       config->alts_list[SCL].alt_fun);
	/* Set GPIO back to I2C alternate function of SDA */
	pinmux_pin_set(config->alts_list[SDA].pinctrls,
		       config->alts_list[SDA].pin,
		       config->alts_list[SDA].alt_fun);

	/* reset i2c port */
	i2c_reset(dev);
	printk("I2C ch%d reset cause %d\n", config->port,
	       I2C_RC_NO_IDLE_FOR_START);

	return 0;
}

static const struct i2c_driver_api i2c_it8xxx2_driver_api = {
	.configure = i2c_it8xxx2_configure,
	.get_config = i2c_it8xxx2_get_config,
	.transfer = i2c_it8xxx2_transfer,
	.recover_bus = i2c_it8xxx2_recover_bus,
};

#define I2C_ITE_IT8XXX2_INIT(idx)                                              \
	static void i2c_it8xxx2_config_func_##idx(void);                       \
	static const struct i2c_alts_cfg                                       \
		i2c_alts_##idx[DT_INST_NUM_PINCTRLS_BY_IDX(idx, 0)] =          \
			IT8XXX2_DT_ALT_ITEMS_LIST(idx);                        \
									       \
	static const struct i2c_it8xxx2_config i2c_it8xxx2_cfg_##idx = {       \
		.base = (uint8_t *)(DT_INST_REG_ADDR(idx)),                    \
		.irq_config_func = i2c_it8xxx2_config_func_##idx,              \
		.bitrate = DT_INST_PROP(idx, clock_frequency),                 \
		.i2c_irq_base = DT_INST_IRQN(idx),                             \
		.port = DT_INST_PROP(idx, port_num),                           \
		.alts_list = i2c_alts_##idx,                                   \
		.gpio_dev = DEVICE_DT_GET(DT_INST_PHANDLE(idx, gpio_dev)),     \
	};                                                                     \
	\
	static struct i2c_it8xxx2_data i2c_it8xxx2_data_##idx;	               \
	\
	I2C_DEVICE_DT_INST_DEFINE(idx,				               \
			i2c_it8xxx2_init, NULL,				       \
			&i2c_it8xxx2_data_##idx,	                       \
			&i2c_it8xxx2_cfg_##idx, POST_KERNEL,		       \
			CONFIG_I2C_INIT_PRIORITY,			       \
			&i2c_it8xxx2_driver_api);                              \
	\
	static void i2c_it8xxx2_config_func_##idx(void)                        \
	{                                                                      \
		IRQ_CONNECT(DT_INST_IRQN(idx),                                 \
			0,                                                     \
			i2c_it8xxx2_isr,                                       \
			DEVICE_DT_INST_GET(idx), 0);                           \
	}

DT_INST_FOREACH_STATUS_OKAY(I2C_ITE_IT8XXX2_INIT)
