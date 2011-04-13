/*
 * archos-lcd-claa-wsvga7.c
 *
 *  Created on: feb 12, 2011
 *      Author: Robic Yvon <robic@archos.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/clk.h>

#include <asm/mach-types.h>

#include <mach/gpio.h>
#include <mach/mux.h>
#include <mach/display.h>
#include <mach/archos-gpio.h>
#include <mach/board-archos.h>
#include <mach/dmtimer.h>

static struct archos_disp_conf display_gpio;
static int panel_state;
static int vcom_val = 130;
static struct omap_dm_timer *vcom_timer;

static int panel_init(struct omap_dss_device *ddata)
{

	pr_debug("panel_init [%s]\n", ddata->name);

	GPIO_INIT_OUTPUT(display_gpio.lcd_pwon);
	GPIO_INIT_OUTPUT(display_gpio.lcd_rst);
	GPIO_INIT_OUTPUT(display_gpio.lvds_en);
	GPIO_INIT_OUTPUT(display_gpio.lcd_avdd_en);

	if (GPIO_EXISTS(display_gpio.lcd_pwon))
		gpio_set_value( GPIO_PIN(display_gpio.lcd_pwon), 1 );

	if (GPIO_EXISTS(display_gpio.lcd_rst))
		gpio_set_value( GPIO_PIN(display_gpio.lcd_rst), 0);

	if (GPIO_EXISTS(display_gpio.lcd_avdd_en))
		gpio_set_value( GPIO_PIN(display_gpio.lcd_avdd_en), 0);

	if (GPIO_EXISTS(display_gpio.lvds_en))
		gpio_set_value( GPIO_PIN(display_gpio.lvds_en), 1);

	// vcom
	omap_cfg_reg(display_gpio.vcom_pwm.mux_cfg);

	msleep(10);
	return 0;
}

static void pwm_set_speed(struct omap_dm_timer *gpt,
		int frequency, int duty_cycle)
{
	u32 val;
	u32 period;
	struct clk *timer_fclk;

	/* and you will have an overflow in 1 sec         */
	/* so,                              */
	/* freq_timer     -> 1s             */
	/* carrier_period -> 1/carrier_freq */
	/* => carrier_period = freq_timer/carrier_freq */

	timer_fclk = omap_dm_timer_get_fclk(gpt);
	period = clk_get_rate(timer_fclk) / frequency;
	val = 0xFFFFFFFF+1-period;
	omap_dm_timer_set_load(gpt, 1, val);

	val = 0xFFFFFFFF+1-(period*duty_cycle/256);
	omap_dm_timer_set_match(gpt, 1, val);

	/* assume overflow first: no toogle if first trig is match */
	omap_dm_timer_write_counter(gpt, 0xFFFFFFFE);
}

static int panel_enable(struct omap_dss_device *disp)
{
	pr_info("panel_enable [%s]\n", disp->name);

	if (GPIO_EXISTS(display_gpio.lcd_pwon))
		gpio_set_value( GPIO_PIN(display_gpio.lcd_pwon), 1 );

	msleep(50);

	if (GPIO_EXISTS(display_gpio.lcd_rst))
		gpio_set_value( GPIO_PIN(display_gpio.lcd_rst), 0);

	if (GPIO_EXISTS(display_gpio.lcd_avdd_en))
		gpio_set_value( GPIO_PIN(display_gpio.lcd_avdd_en), 0);

	msleep(35);

	if (GPIO_EXISTS(display_gpio.lvds_en))
		gpio_set_value( GPIO_PIN(display_gpio.lvds_en), 1);

	vcom_timer = omap_dm_timer_request_specific(display_gpio.vcom_pwm.timer);

	if (vcom_timer != NULL) {
		omap_dm_timer_set_source(vcom_timer, OMAP_TIMER_SRC_SYS_CLK);
	} else {
		printk( "failed to request vcom pwm timer %d \n", display_gpio.vcom_pwm.timer);
	}

	msleep(10);

	panel_state = 1;

	if (vcom_timer != NULL) {
		omap_dm_timer_set_pwm( vcom_timer, 1, 1, OMAP_TIMER_TRIGGER_OVERFLOW_AND_COMPARE);

		pwm_set_speed(vcom_timer, 30000, vcom_val);
		omap_dm_timer_start(vcom_timer);
	}

	return 0;
}

static void panel_disable(struct omap_dss_device *disp)
{
	pr_info("panel_disable [%s]\n", disp->name);

	if (GPIO_EXISTS(display_gpio.lvds_en))
		gpio_set_value( GPIO_PIN(display_gpio.lvds_en), 0);

	if (GPIO_EXISTS(display_gpio.lcd_avdd_en))
		gpio_set_value( GPIO_PIN(display_gpio.lcd_avdd_en), 1);

	msleep(10);

	if (GPIO_EXISTS(display_gpio.lcd_pwon))
		gpio_set_value( GPIO_PIN(display_gpio.lcd_pwon), 0 );

	if (GPIO_EXISTS(display_gpio.lcd_rst))
		gpio_set_value( GPIO_PIN(display_gpio.lcd_rst), 1);

	if (vcom_timer != NULL) {
		omap_dm_timer_stop(vcom_timer);
		omap_dm_timer_free(vcom_timer);
	}
	panel_state = 0;
}

static int panel_set_vcom(struct omap_dss_device *disp, u32 vcom)
{
	pr_debug("panel_set_vcom [%s]\n", disp->name);

	vcom_val = vcom;
	if ((panel_state == 1) && (vcom_timer != NULL))
		pwm_set_speed(vcom_timer, 30000, vcom_val);

	return 0;
}

static int panel_get_vcom(struct omap_dss_device *disp)
{
	pr_debug("panel_get_vcom [%s]\n", disp->name);

	return vcom_val;
}

static struct omap_dss_device claa_wsvga_7_panel = {
	.type			= OMAP_DISPLAY_TYPE_DPI,
	.name			= "lcd",
	.driver_name		= "claa_wsvga_7",
	.phy.dpi.data_lines	= 24,
	.platform_enable	= panel_enable,
	.platform_disable	= panel_disable,
	.get_vcom		= panel_get_vcom,
	.set_vcom		= panel_set_vcom,
};

int __init panel_claa_wsvga_7_init(struct omap_dss_device *disp_data)
{
	const struct archos_display_config *disp_cfg;
	int ret = -ENODEV;

	printk(KERN_INFO "panel_claa_wsvga_7_init\n");

	disp_cfg = omap_get_config( ARCHOS_TAG_DISPLAY, struct archos_display_config );
	if (disp_cfg == NULL)
		return ret;

	if ( hardware_rev >= disp_cfg->nrev ) {
		printk(KERN_DEBUG "archos_display_init: hardware_rev (%i) >= nrev (%i)\n",
			hardware_rev, disp_cfg->nrev);
		return ret;
	}

	display_gpio = disp_cfg->rev[hardware_rev];

	panel_init(&claa_wsvga_7_panel);
	*disp_data = claa_wsvga_7_panel;

	return 0;
}
