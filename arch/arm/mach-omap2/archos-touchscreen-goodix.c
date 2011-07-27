/*
 *    archos-touchscreen-goodix.c : 17/02/2011
 *    g.revaillot, revaillot@archos.com
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/err.h>

#include <asm/mach-types.h>
#include <mach/archos-gpio.h>
#include <mach/board-archos.h>
#include <mach/board.h>
#include <mach/gpio.h>
#include <mach/mux.h>

#include <linux/goodix-gt80x.h>

static struct archos_gpio ts_irq = UNUSED_GPIO;
static struct archos_gpio ts_pwron = UNUSED_GPIO;
static struct archos_gpio ts_shtdwn = UNUSED_GPIO;

static void set_power(int on_off)
{
	gpio_set_value( GPIO_PIN( ts_pwron ), on_off);
}

static void set_shutdown(int on_off)
{
	gpio_set_value( GPIO_PIN( ts_shtdwn ), on_off);
}

int __init archos_touchscreen_goodix_init(struct goodix_gt80x_platform_data *pdata)
{
	const struct archos_i2c_tsp_config *tsp_cfg = 
		omap_get_config(ARCHOS_TAG_I2C_TSP, struct archos_i2c_tsp_config);

	if (tsp_cfg == NULL)
		return -ENODEV;

	if ( hardware_rev >= tsp_cfg->nrev ) {
		printk(KERN_DEBUG "%s: hardware_rev (%i) >= nrev (%i)\n",
			__FUNCTION__, hardware_rev, tsp_cfg->nrev);
		return -ENODEV;
	}

	ts_pwron = tsp_cfg->rev[hardware_rev].pwr_gpio;
	archos_gpio_init_output(&ts_pwron, "goodix_ts_pwron");

	ts_shtdwn = tsp_cfg->rev[hardware_rev].shtdwn_gpio;
	archos_gpio_init_output(&ts_shtdwn, "goodix_ts_shtdwn");

	ts_irq = tsp_cfg->rev[hardware_rev].irq_gpio;
	archos_gpio_init_input(&ts_irq, "goodix_ts_irq");

	if (GPIO_PIN(ts_irq) != -1) {
		pdata->irq = gpio_to_irq(GPIO_PIN(ts_irq));
	} else {
		pdata->irq = -1;
	}

	pdata->set_power = &set_power;
	pdata->set_shutdown = &set_shutdown;

	// MTU units have their X axis inverted and need init v1.
	if (hardware_rev < 2) {
		pdata->init_version = 1;
		pdata->orientation = GOODIX_GT80X_ORIENTATION_INV_X;
	}

	printk(KERN_DEBUG "%s: irq_gpio %d - irq %d, pwr_gpio %d, shtdwn_gpio %d\n",
			__FUNCTION__, pdata->irq, ts_irq.nb, ts_pwron.nb, ts_shtdwn.nb);

	return 0;
}

