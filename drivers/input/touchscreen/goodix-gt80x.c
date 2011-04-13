/*
 *    goodix-gt80x.c : 17/02/2011
 *    g.revaillot, revaillot@archos.com
 */

#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <linux/goodix-gt80x.h>

#define DMP_EV if(0)

#define NB_POINTS 5

#define REG_CONF_BASE	0x30
#define REG_X_MAX	0x3a
#define REG_Y_MAX	0x3c
#define REG_CMD		0x69
#define REG_BUFFER	0x6a

struct goodix_register_map {
	u8 flags;
	u8 states;
	struct {
		u8 x_h;
		u8 x_l;
		u8 y_h;
		u8 y_l;
		u8 pressure;
	} p[NB_POINTS];
	u8 checksum;
} d = {
	.flags = 0x00,
	.states = 0x01,
	.p = {
		{ 0x02, 0x03, 0x04, 0x05, 0x06 },
		{ 0x07, 0x08, 0x09, 0x0a, 0x0b },
		{ 0x0c, 0x0d, 0x0e, 0x0f, 0x10 },
		{ 0x11, 0x18, 0x19, 0x1a, 0x1b },
		{ 0x1c, 0x1d, 0x1e, 0x1f, 0x20 },
	},
	.checksum = 0x21,
};

#if 0
u8 twl6030_init_sequence[] = {
	0x19, 0x05, 0x06, 0x28, 0x02, 0x14, 0x14, 0x10,
	0x50, 0xB8, 0x14, 0x00, 0x1E, 0x00, 0x01, 0x23,
	0x45, 0x67, 0x89, 0xAB, 0xCD, 0xE1, 0x00, 0x00,
	0x00, 0x00, 0x0D, 0xCF, 0x20, 0x03, 0x05, 0x83,
	0x50, 0x3C, 0x1E, 0xB4, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 
	0x01,
};
#else
u8 twl6030_init_sequence[] = {
	// 0x30 -> 0x37
	0x19, 0x05, 0x03, 0x28, 0x02, 0x14, 0x40, 0x10,
	// 0x38 -> 0x3f
	0x3C, 0xF8, 0x14, 0x00, 0x1E, 0x00, 0x01, 0x23,
	// 0x40 -> 0x47
	0x45, 0x67, 0x89, 0xAB, 0xCD, 0xE1, 0x00, 0x00,
	// 0x48 -> 0x4f
	0x00, 0x00, 0x4D, 0xC0, 0x20, 0x01, 0x01, 0x83,
	// 0x50 -> 0x57
	0x50, 0x3C, 0x1E, 0xB4, 0x00, 0x0A, 0x3C, 0x6E,
	// 0x58 -> 0x5f
	0x1E, 0x00, 0x50, 0x32, 0x73, 0x00, 0x00, 0x00,
	// 0x60 -> 0x63
	0x00, 0x00, 0x00, 0x00,
	// 0x64 : refresh.
	0x01,
};
#endif

struct goodix_gt80x_priv {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct workqueue_struct *wq;

	int irq;

	struct work_struct work;

	void (*set_power)(int on);
	void (*set_shutdown)(int on);

	int flags;
	int orientation;

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif

	int state[NB_POINTS];
};

enum {
	ST_RELEASED = 0,
	ST_PRESSED = 1,
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void goodix_gt80x_early_suspend(struct early_suspend *h);
static void goodix_gt80x_late_resume(struct early_suspend *h);
#endif

static void goodix_gt80x_shutdown(struct i2c_client *client, int on_off)
{
	struct goodix_gt80x_priv *priv = i2c_get_clientdata(client);

	if (priv->set_shutdown)
		priv->set_shutdown(on_off);
}

static void goodix_gt80x_power(struct i2c_client *client, int on_off)
{
	struct goodix_gt80x_priv *priv = i2c_get_clientdata(client);

	// shutdown line must be down when power is cut to avoid leak.
	if (on_off == 0)
		goodix_gt80x_shutdown(client, 0);

	if (priv->set_power)
		priv->set_power(on_off);
}

static int goodix_gt80x_write(struct i2c_client * client, u8 addr, u8 *value, u8 len)
{
	struct i2c_msg msg;
	int ret;

	char *buff = kzalloc(sizeof(addr) + len, GFP_KERNEL); 

	if (!buff)
		return -ENOMEM;

	*buff = addr;

	memcpy(buff + sizeof(addr), value, len);

	msg.addr = client->addr;
	msg.flags = 0;
	msg.buf = buff;
	msg.len = sizeof(addr) + len;

	ret = i2c_transfer(client->adapter, &msg, 1);

	kfree(buff);

	if (ret <= 0)
		return -EIO;

	return len;
}

static inline int goodix_gt80x_write_u8(struct i2c_client * client, u8 addr, u8 value) {
	return goodix_gt80x_write(client, addr, &value, sizeof(u8));
}


static int goodix_gt80x_read(struct i2c_client * client, u8 addr, u8 *value, u8 len)
{
	struct i2c_msg msg[2];
	int ret;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &addr;
	msg[0].len = sizeof(addr);

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = value;
	msg[1].len = len;

	ret = i2c_transfer(client->adapter, msg, 2);

	if (ret == 2)
		return len;
	else if (ret >= 0){
		return -EIO;
	} else {
		return ret;
	}
}

static inline int goodix_gt80x_read_u8(struct i2c_client * client, u8 addr, u8 *value) {
	return goodix_gt80x_read(client, addr, value, sizeof(u8));
}

static inline int goodix_gt80x_read_u16(struct i2c_client * client, u8 addr, u16 *value)
{
	u8 buf[2];
	int ret;

	ret = goodix_gt80x_read(client, addr, buf, sizeof(buf));

	*value = (buf[0] << 8) | buf[1];

	return ret; 
}

#if 0
static int goodix_gt80x_write_cmd(struct i2c_client * client, u8 cmd_val)
{
	return goodix_gt80x_write_u8(client, REG_CMD, cmd_val);
}

static void dmp(struct i2c_client *client, u8 start, u8 len)
{
	u8 buff[256];

	if (goodix_gt80x_read(client, start, (char*) &buff, len) != len) {
		printk("%s : fail\n", __FUNCTION__);
	} else {
		int u;
		printk("\n");
		for (u=0; u < len; u++) {
			switch(start + u) {
				case 0x00: printk("%15s", "PointFlags"); break;
				case 0x01: printk("%15s", "PointStates"); break;

				case 0x02: printk("%15s", "x0h"); break;
				case 0x03: printk("%15s", "x0l"); break;
				case 0x04: printk("%15s", "y0h"); break;
				case 0x05: printk("%15s", "y0l"); break;
				case 0x06: printk("%15s", "p0 pressure"); break;

				case 0x07: printk("%15s", "x1h"); break;
				case 0x08: printk("%15s", "x1l"); break;
				case 0x09: printk("%15s", "y1h"); break;
				case 0x0a: printk("%15s", "y1l"); break;
				case 0x0b: printk("%15s", "p1 pressure"); break;

				case 0x0c: printk("%15s", "x2h"); break;
				case 0x0d: printk("%15s", "x2l"); break;
				case 0x0e: printk("%15s", "y2h"); break;
				case 0x0f: printk("%15s", "y2l"); break;
				case 0x10: printk("%15s", "p2 pressure"); break;

				case 0x11: printk("%15s", "x3h"); break;
				case 0x18: printk("%15s", "x3l"); break;
				case 0x19: printk("%15s", "y3h"); break;
				case 0x1a: printk("%15s", "y3l"); break;
				case 0x1b: printk("%15s", "p3 pressure"); break;

				case 0x1c: printk("%15s", "x4h"); break;
				case 0x1d: printk("%15s", "x4l"); break;
				case 0x1e: printk("%15s", "y4h"); break;
				case 0x1f: printk("%15s", "y4l"); break;
				case 0x20: printk("%15s", "p4 pressure"); break;

				case 0x21: printk("%15s", "checksum"); break;

				case 0x39: printk("%15s", "ModuleSwitch"); break;
				case 0x64: printk("%15s", "ConfigFresh"); break;

				default: printk("%15s", "?");
			}

			printk("/0x%02x = 0x%02x ", u+start, buff[u]);

			if ((u+1) % 8 == 0)
				printk("\n");
			else
				printk(" ");
		}
		printk("\n");
	}
}
#endif

static void goodix_gt80x_work_func(struct work_struct *work)
{
	struct goodix_gt80x_priv *priv =
		container_of(work, struct goodix_gt80x_priv, work);
	u8 regs[0x22];
	int ret;
	int i = 0;

	// dump the whole output registers array in one burst read.
	// IRQ is released when read is finished : we can not do multiple
	// reads based on pointer states : irq will trigger too soon.
	ret = goodix_gt80x_read(priv->client, 0, regs, sizeof(regs));

	if (ret != sizeof(regs)) {
		dev_err(&priv->client->dev, "%s: could not read output regs.\n",
				__FUNCTION__);
		goto exit_work;
	}

	DMP_EV dev_err(&priv->client->dev, "F0x%02x S0x%02x ",
			regs[d.flags], regs[d.states]);

	for (i = 0; i < NB_POINTS; i++) {

		DMP_EV printk("- %i:%d x%06d/y%06d/p%03d",
				i, !!(regs[d.flags] & (1 << i)),
				(regs[d.p[i].x_h] << 8) | regs[d.p[i].x_l],
				(regs[d.p[i].y_h] << 8) | regs[d.p[i].y_l],
				regs[d.p[i].pressure]);

		// pointer state released, but reported as pressed ? 
		// update state and start report if single touch compat is on..
		if ((priv->state[i] == ST_RELEASED) && (regs[d.flags] & (1 << i))) {
			priv->state[i] = ST_PRESSED;

			// issue key_press;
#ifdef SINGLETOUCH_COMPAT 
			if (i==1)
				input_report_key(priv->input_dev, BTN_TOUCH, 1);
#endif

			// continue process
		}

		if (priv->state[i] == ST_PRESSED) {
			u8 p = regs[d.p[i].pressure];

			if ((regs[d.flags] & (1 << i)) == 0) {
				// pointer state pressed, but reported as released ?

				// issue rey_release and sync
				input_report_abs(priv->input_dev, ABS_MT_TRACKING_ID, i);
				input_report_abs(priv->input_dev, ABS_MT_TOUCH_MAJOR, p);

				input_mt_sync(priv->input_dev);
#ifdef SINGLETOUCH_COMPAT
				if (i==1)
					input_report_key(priv->input_dev, BTN_TOUCH, 0);
#endif
				priv->state[i] = ST_RELEASED;
			} else {
				u16 x, y;

				if (priv->flags & GOODIX_GT80X_FLAGS_XY_SWAP) {
					y = (regs[d.p[i].x_h] << 8) | regs[d.p[i].x_l];
					x = (regs[d.p[i].y_h] << 8) | regs[d.p[i].y_l];
				} else {
					x = (regs[d.p[i].x_h] << 8) | regs[d.p[i].x_l];
					y = (regs[d.p[i].y_h] << 8) | regs[d.p[i].y_l];
				}
				
				// issue point coords and sync
				input_report_abs(priv->input_dev, ABS_MT_TRACKING_ID, i);

				input_report_abs(priv->input_dev, ABS_MT_POSITION_X, x);
				input_report_abs(priv->input_dev, ABS_MT_POSITION_Y, y);

				input_report_abs(priv->input_dev, ABS_MT_TOUCH_MAJOR, p);
				input_mt_sync(priv->input_dev);

#ifdef SINGLETOUCH_COMPAT
				if (i==1) {
					input_report_abs(priv->input_dev, ABS_X, x);
					input_report_abs(priv->input_dev, ABS_Y, y);
					input_report_abs(priv->input_dev, ABS_PRESSURE, p);
				}
#endif
			}
		}
	}

	input_sync(priv->input_dev);

	DMP_EV printk("\n");

exit_work:
	enable_irq(priv->irq);
}

static irqreturn_t goodix_gt80x_irq_handler(int irq, void * p)
{
	struct goodix_gt80x_priv *priv = p;

	disable_irq_nosync(priv->irq);

	queue_work(priv->wq, &priv->work);

	return IRQ_HANDLED;
}

static int goodix_gt80x_startup_sequence(struct i2c_client *client)
{
	struct goodix_gt80x_priv *priv = i2c_get_clientdata(client);
	int retry = 10;
	int ret;

	u8 buf;

	// hw reset.
	goodix_gt80x_power(client, 0);
	msleep(1);
	goodix_gt80x_power(client, 1);

	// startup sequence
	goodix_gt80x_shutdown(client, 1);
	msleep(100);
	goodix_gt80x_shutdown(client, 0);

	// allow tsp to take some time to startup.
	while ((goodix_gt80x_read(client, 0, &buf, sizeof(buf)) != sizeof(buf)) && (--retry)) {
		dev_info(&client->dev, "%s: ping ? (%d)\n", __FUNCTION__, retry);
		msleep(25);
	}

	if (!retry) {
		dev_err(&client->dev, "%s : detect failed\n", __FUNCTION__);
		return -ENODEV;
	}

	// vendor provided init sequence. Should eventually be part
	// of platform data since not controller specific, but more
	// related to TSP/Device context.
	ret = goodix_gt80x_write(client, REG_CONF_BASE, twl6030_init_sequence,
			sizeof(twl6030_init_sequence));

	if (ret != sizeof(twl6030_init_sequence))
		return -ENODEV;

	msleep(20);

	if (goodix_gt80x_write_u8(client, 0x68, priv->orientation) != sizeof(u8))
		return -ENODEV;

	return 0;
}

static int goodix_gt80x_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct goodix_gt80x_platform_data *pdata = client->dev.platform_data;
	struct goodix_gt80x_priv *priv;
	u16 x_max, y_max;

	int ret = 0;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (priv == NULL)
		return -ENOMEM;

	i2c_set_clientdata(client, priv);

	priv->client = client;

	if (pdata) {
		priv->irq = pdata->irq;

		priv->set_power = pdata->set_power;
		priv->set_shutdown = pdata->set_shutdown;

		priv->orientation = pdata->orientation;
		priv->flags = pdata->flags;
	}

	priv->wq = create_singlethread_workqueue(id->name);
	if (priv->wq == NULL) {
		ret = -ENOMEM;
		goto err_wq_create;
	}

	priv->input_dev = input_allocate_device();
	if (priv->input_dev == NULL) {
		ret = -ENOMEM;
		goto err_input_alloc_failed;
	}

	ret = goodix_gt80x_startup_sequence(client);
	if (ret < 0)
		goto err_detect_failed;

	INIT_WORK(&priv->work, goodix_gt80x_work_func);

	priv->input_dev->name = id->name;

	set_bit(EV_SYN, priv->input_dev->evbit);

	set_bit(EV_KEY, priv->input_dev->evbit);
	set_bit(BTN_TOUCH, priv->input_dev->keybit);
	set_bit(EV_ABS, priv->input_dev->evbit);

	set_bit(ABS_MT_POSITION_X, priv->input_dev->absbit);
	set_bit(ABS_MT_POSITION_Y, priv->input_dev->absbit);
	set_bit(ABS_MT_TRACKING_ID, priv->input_dev->absbit);
	set_bit(ABS_MT_TOUCH_MAJOR, priv->input_dev->absbit);

	if (goodix_gt80x_read_u16(client, REG_X_MAX, &x_max) != sizeof(u16)) {
		ret = -ENODEV;
		goto err_setup_failed;
	}

	if (goodix_gt80x_read_u16(client, REG_Y_MAX, &y_max) != sizeof(u16)) {
		ret = -ENODEV;
		goto err_setup_failed;
	}

	if (priv->flags & GOODIX_GT80X_FLAGS_XY_SWAP) {
		input_set_abs_params(priv->input_dev, ABS_Y, 0, x_max, 0, 0);
		input_set_abs_params(priv->input_dev, ABS_X, 0, y_max, 0, 0);
		input_set_abs_params(priv->input_dev, ABS_MT_POSITION_Y, 0, x_max, 0, 0);
		input_set_abs_params(priv->input_dev, ABS_MT_POSITION_X, 0, y_max, 0, 0);
	} else {
		input_set_abs_params(priv->input_dev, ABS_X, 0, x_max, 0, 0);
		input_set_abs_params(priv->input_dev, ABS_Y, 0, y_max, 0, 0);
		input_set_abs_params(priv->input_dev, ABS_MT_POSITION_X, 0, x_max, 0, 0);
		input_set_abs_params(priv->input_dev, ABS_MT_POSITION_Y, 0, y_max, 0, 0);
	}

	input_set_abs_params(priv->input_dev, ABS_PRESSURE, 0, 0xff, 0, 0);

	ret = input_register_device(priv->input_dev);
	if (ret) {
		ret = -ENODEV;
		dev_err(&client->dev, "%s: Unable to register %s input device\n",
				__FUNCTION__, priv->input_dev->name);
		goto err_input_register_failed;
	}

	ret = request_irq(priv->irq, goodix_gt80x_irq_handler, IRQF_TRIGGER_RISING,
				client->name, priv);
	if (ret) {
		ret = -ENODEV;
		dev_err(&client->dev, "request_irq failed\n");
		goto err_irq_request_failed;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	priv->early_suspend.level = EARLY_SUSPEND_LEVEL_DISABLE_FB;
	priv->early_suspend.suspend = goodix_gt80x_early_suspend;
	priv->early_suspend.resume = goodix_gt80x_late_resume;
	register_early_suspend(&priv->early_suspend);
#endif

	return 0;

err_irq_request_failed:
err_input_register_failed:

err_setup_failed:
	dev_err(&client->dev, "setup failed.\n");

err_detect_failed:
	goodix_gt80x_power(client, 0);
	input_free_device(priv->input_dev);

err_input_alloc_failed:
	destroy_workqueue(priv->wq);

err_wq_create:
	kfree(priv);

	return ret;
}

static int goodix_gt80x_remove(struct i2c_client *client)
{
	struct goodix_gt80x_priv *priv = i2c_get_clientdata(client);
#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&priv->early_suspend);
#endif
	destroy_workqueue(priv->wq);

	free_irq(priv->irq, priv);

	input_unregister_device(priv->input_dev);

	goodix_gt80x_power(client, 0);

	kfree(priv);
	return 0;
}

static int goodix_gt80x_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct goodix_gt80x_priv *priv = i2c_get_clientdata(client);

	disable_irq(priv->irq);

	flush_work(&priv->work);

	goodix_gt80x_power(client, 0);

	return 0;
}

static int goodix_gt80x_resume(struct i2c_client *client)
{
	struct goodix_gt80x_priv *priv = i2c_get_clientdata(client);

	if (goodix_gt80x_startup_sequence(client) < 0)
		printk("%s: failed ?\n", __FUNCTION__);

	enable_irq(priv->irq);

	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void goodix_gt80x_early_suspend(struct early_suspend *h)
{
	struct goodix_gt80x_priv *priv =
		container_of(h, struct goodix_gt80x_priv, early_suspend);
	goodix_gt80x_suspend(priv->client, PMSG_SUSPEND);
}

static void goodix_gt80x_late_resume(struct early_suspend *h)
{
	struct goodix_gt80x_priv *priv =
		container_of(h, struct goodix_gt80x_priv, early_suspend);
	goodix_gt80x_resume(priv->client);
}
#endif

static const struct i2c_device_id goodix_gt80x_id[] = {
	{ GOODIX_GT801_NAME, 0 },
	{ }
};

static struct i2c_driver goodix_gt80x_driver = {
	.probe		= goodix_gt80x_probe,
	.remove		= goodix_gt80x_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend	= goodix_gt80x_suspend,
	.resume		= goodix_gt80x_resume,
#endif
	.id_table	= goodix_gt80x_id,
	.driver = {
		.name	= GOODIX_GT801_NAME,
	},
};

static int __init goodix_gt80x_init(void)
{
	return i2c_add_driver(&goodix_gt80x_driver);
}

static void __exit goodix_gt80x_exit(void)
{
	i2c_del_driver(&goodix_gt80x_driver);
}

module_init(goodix_gt80x_init);
module_exit(goodix_gt80x_exit);

MODULE_AUTHOR("Guillaume Revaillot <revaillot@archos.com>");
MODULE_DESCRIPTION("Goodix GT80x Touchscreen Driver");
MODULE_LICENSE("GPL");
