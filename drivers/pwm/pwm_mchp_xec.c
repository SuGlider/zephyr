/* pwm_mchp_xec.c - Microchip XEC PWM driver */

#define DT_DRV_COMPAT microchip_xec_pwm

/*
 * Copyright (c) 2019 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_REGISTER(pwm_mchp_xec, CONFIG_PWM_LOG_LEVEL);

#include <drivers/pwm.h>
#include <soc.h>
#include <errno.h>

#include <stdlib.h>

/* Minimal on/off are 1 & 1 both are incremented, so 4.
 * 0 cannot be set (used for full low/high output) so a
 * combination of on_off of 2 is not possible.
 */
#define XEC_PWM_LOWEST_ON_OFF	4U
/* Maximal on/off are UINT16_T, both are incremented.
 * Multiplied by the highest divider: 16
 */
#define XEC_PWM_HIGHEST_ON_OFF	(2U * (UINT16_MAX + 1U) * 16U)

#define XEC_PWM_MIN_HIGH_CLK_FREQ			\
	(MCHP_PWM_INPUT_FREQ_HI / XEC_PWM_HIGHEST_ON_OFF)
#define XEC_PWM_MAX_LOW_CLK_FREQ			\
	(MCHP_PWM_INPUT_FREQ_LO / XEC_PWM_LOWEST_ON_OFF)
/* Precision factor for frequency calculation
 * To mitigate frequency comparision up to the first digit after 0.
 */
#define XEC_PWM_FREQ_PF		10U
/* Precision factor for DC calculation
 * To avoid losing some digits after 0.
 */
#define XEC_PWM_DC_PF		100000U
/* Lowest reachable frequency */
#define XEC_PWM_FREQ_LIMIT	1 /* 0.1hz * XEC_PWM_FREQ_PF */

struct pwm_xec_config {
	uint32_t base_address;
};

#define PWM_XEC_REG_BASE(_dev)				\
	((PWM_Type *)			\
	 ((const struct pwm_xec_config * const)		\
	  _dev->config)->base_address)

#define PWM_XEC_CONFIG(_dev)				\
	(((const struct pwm_xec_config * const)		\
	  _dev->config))

struct xec_params {
	uint32_t on;
	uint32_t off;
	uint8_t div;
};

#define NUM_DIV_ELEMS		16

uint32_t max_freq_high_on_div[NUM_DIV_ELEMS] = {
	48000000,
	24000000,
	16000000,
	12000000,
	9600000,
	8000000,
	6857142,
	6000000,
	5333333,
	4800000,
	4363636,
	4000000,
	3692307,
	3428571,
	3200000,
	3000000
};

uint32_t max_freq_low_on_div[NUM_DIV_ELEMS] = {
	100000,
	50000,
	33333,
	25000,
	20000,
	16666,
	14285,
	12500,
	11111,
	10000,
	9090,
	8333,
	7692,
	7142,
	6666,
	6250
};

static uint32_t xec_compute_frequency(uint32_t clk, uint32_t on, uint32_t off)
{
	return ((clk * XEC_PWM_FREQ_PF)/((on + 1) + (off + 1)));
}

static uint16_t xec_select_div(uint32_t freq, uint32_t max_freq[16])
{
	uint8_t i;

	if (freq >= max_freq[3]) {
		return 0;
	}

	freq *= XEC_PWM_LOWEST_ON_OFF;

	for (i = 0; i < 15; i++) {
		if (freq >= max_freq[i]) {
			break;
		}
	}

	return i;
}

static void xec_compute_on_off(uint32_t freq, uint32_t dc, uint32_t clk,
			       uint32_t *on, uint32_t *off)
{
	uint64_t on_off;

	on_off = (clk * 10) / freq;

	*on = ((on_off * dc) / XEC_PWM_DC_PF) - 1;
	*off = on_off - *on - 2;
}

static uint32_t xec_compute_dc(uint32_t on, uint32_t off)
{
	int dc = (on + 1) + (off + 1);

	/* Make calculation in uint64_t since XEC_PWM_DC_PF is large */
	dc = (((uint64_t)(on + 1) * XEC_PWM_DC_PF) / dc);

	return (uint32_t)dc;
}

static uint16_t xec_compare_div_on_off(uint32_t target_freq, uint32_t dc,
				    uint32_t max_freq[16],
				    uint8_t div_a, uint8_t div_b,
				    uint32_t *on_a, uint32_t *off_a)
{
	uint32_t freq_a, freq_b, on_b, off_b;

	xec_compute_on_off(target_freq, dc, max_freq[div_a],
			   on_a, off_a);

	freq_a = xec_compute_frequency(max_freq[div_a], *on_a, *off_a);

	xec_compute_on_off(target_freq, dc, max_freq[div_b],
			   &on_b, &off_b);

	freq_b = xec_compute_frequency(max_freq[div_b], on_b, off_b);

	if ((target_freq - freq_a) < (target_freq - freq_b)) {
		if ((*on_a <= UINT16_MAX) && (*off_a <= UINT16_MAX)) {
			return div_a;
		}
	}

	if ((on_b <= UINT16_MAX) && (off_b <= UINT16_MAX)) {
		*on_a = on_b;
		*off_a = off_b;

		return div_b;
	}

	return div_a;
}

static uint8_t xec_select_best_div_on_off(uint32_t target_freq, uint32_t dc,
					uint32_t max_freq[16],
					uint32_t *on, uint32_t *off)
{
	int div_comp;
	uint8_t div;

	div = xec_select_div(target_freq, max_freq);

	for (div_comp = (int)div - 1; div_comp >= 0; div_comp--) {
		div = xec_compare_div_on_off(target_freq, dc, max_freq,
					     div, div_comp, on, off);
	}

	return div;
}

static struct xec_params *xec_compare_params(uint32_t target_freq,
					     struct xec_params *hc_params,
					     struct xec_params *lc_params)
{
	struct xec_params *params;
	uint32_t freq_h = 0;
	uint32_t freq_l = 0;

	if (hc_params->div < NUM_DIV_ELEMS) {
		freq_h = xec_compute_frequency(
				max_freq_high_on_div[hc_params->div],
				hc_params->on,
				hc_params->off);
	}

	if (lc_params->div < NUM_DIV_ELEMS) {
		freq_l = xec_compute_frequency(
				max_freq_low_on_div[lc_params->div],
				lc_params->on,
				lc_params->off);
	}

	if (abs((int)target_freq - (int)freq_h) <
	    abs((int)target_freq - (int)freq_l)) {
		params = hc_params;
	} else {
		params = lc_params;
	}

	LOG_DBG("\tFrequency (x%u): %u", XEC_PWM_FREQ_PF, freq_h);
	LOG_DBG("\tOn %s clock, ON %u OFF %u DIV %u",
		params == hc_params ? "High" : "Low",
		params->on, params->off, params->div);

	return params;
}

static void xec_compute_and_set_parameters(const struct device *dev,
					   uint32_t target_freq,
					   uint32_t on, uint32_t off)
{
	PWM_Type *pwm_regs = PWM_XEC_REG_BASE(dev);
	bool compute_high, compute_low;
	struct xec_params hc_params;
	struct xec_params lc_params;
	struct xec_params *params;
	uint32_t dc, reg;

	dc = xec_compute_dc(on, off);

	compute_high = (target_freq >= XEC_PWM_MIN_HIGH_CLK_FREQ);
	compute_low = (target_freq <= XEC_PWM_MAX_LOW_CLK_FREQ);

	LOG_DBG("Target freq (x%u): %u and DC %u per-cent",
		XEC_PWM_FREQ_PF, target_freq, (dc / 1000));

	if (compute_high) {
		if (!compute_low
		    && (on <= UINT16_MAX)
		    && (off <= UINT16_MAX)) {
			hc_params.on = on;
			hc_params.off = off;
			hc_params.div = 0;
			lc_params.div = UINT8_MAX;

			goto done;
		}

		hc_params.div = xec_select_best_div_on_off(
					target_freq, dc,
					max_freq_high_on_div,
					&hc_params.on,
					&hc_params.off);
		LOG_DBG("Best div high: %u (on/off: %u/%u)",
			hc_params.div, hc_params.on, hc_params.off);
	} else {
		hc_params.div = UINT8_MAX;
	}

	if (compute_low) {
		lc_params.div = xec_select_best_div_on_off(
					target_freq, dc,
					max_freq_low_on_div,
					&lc_params.on,
					&lc_params.off);
		LOG_DBG("Best div low: %u (on/off: %u/%u)",
			lc_params.div, lc_params.on, lc_params.off);
	} else {
		lc_params.div = UINT8_MAX;
	}
done:
	pwm_regs->CONFIG &= ~MCHP_PWM_CFG_ENABLE;

	reg = pwm_regs->CONFIG;

	params = xec_compare_params(target_freq, &hc_params, &lc_params);
	if (params == &hc_params) {
		reg |= MCHP_PWM_CFG_CLK_SEL_48M;
	} else {
		reg |= MCHP_PWM_CFG_CLK_SEL_100K;
	}

	pwm_regs->COUNT_ON = params->on;
	pwm_regs->COUNT_OFF = params->off;
	reg |= MCHP_PWM_CFG_CLK_PRE_DIV(params->div);
	reg |= MCHP_PWM_CFG_ENABLE;

	pwm_regs->CONFIG = reg;
}

static int pwm_xec_pin_set(const struct device *dev, uint32_t pwm,
			   uint32_t period_cycles, uint32_t pulse_cycles,
			   pwm_flags_t flags)
{
	PWM_Type *pwm_regs = PWM_XEC_REG_BASE(dev);
	uint32_t target_freq;
	uint32_t on, off;

	if (pwm > 0) {
		return -EIO;
	}

	if (pulse_cycles > period_cycles) {
		return -EINVAL;
	}

	if (flags) {
		/* PWM polarity not supported (yet?) */
		return -ENOTSUP;
	}

	on = pulse_cycles;
	off = period_cycles - pulse_cycles;

	target_freq = xec_compute_frequency(MCHP_PWM_INPUT_FREQ_HI, on, off);
	if (target_freq < XEC_PWM_FREQ_LIMIT) {
		LOG_DBG("Target frequency below limit");
		return -EINVAL;
	}

	if ((pulse_cycles == 0U) && (period_cycles == 0U)) {
		pwm_regs->CONFIG &= ~MCHP_PWM_CFG_ENABLE;
	} else if ((pulse_cycles == 0U) && (period_cycles > 0U)) {
		pwm_regs->COUNT_ON = 0;
		pwm_regs->COUNT_OFF = 1;
	} else if ((pulse_cycles > 0U) && (period_cycles == 0U)) {
		pwm_regs->COUNT_ON = 1;
		pwm_regs->COUNT_OFF = 0;
	} else {
		xec_compute_and_set_parameters(dev, target_freq, on, off);
	}

	return 0;
}

static int pwm_xec_get_cycles_per_sec(const struct device *dev, uint32_t pwm,
				      uint64_t *cycles)
{
	ARG_UNUSED(dev);

	if (pwm > 0) {
		return -EIO;
	}

	if (cycles) {
		/* User does not have to know about lowest clock,
		 * the driver will select the most relevant one.
		 */
		*cycles = MCHP_PWM_INPUT_FREQ_HI;
	}

	return 0;
}

static int pwm_xec_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static struct pwm_driver_api pwm_xec_api = {
	.pin_set = pwm_xec_pin_set,
	.get_cycles_per_sec = pwm_xec_get_cycles_per_sec
};

#define XEC_INST_INIT(inst)						\
	static struct pwm_xec_config pwm_xec_dev_config_##inst = {	\
		.base_address = DT_INST_REG_ADDR(inst)			\
	};								\
									\
	DEVICE_DT_INST_DEFINE(inst,					\
			    pwm_xec_init,				\
			    NULL,					\
			    NULL,					\
			    &pwm_xec_dev_config_##inst,			\
			    POST_KERNEL,				\
			    CONFIG_KERNEL_INIT_PRIORITY_DEVICE,		\
			    &pwm_xec_api);

DT_INST_FOREACH_STATUS_OKAY(XEC_INST_INIT)
