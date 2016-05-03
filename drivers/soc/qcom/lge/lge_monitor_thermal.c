/* Copyright (c) 2013-2014, LG Eletronics. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/percpu.h>
#include <linux/of.h>
#include <linux/cpu.h>
#include <linux/platform_device.h>
#include <soc/qcom/scm.h>
#include <soc/qcom/memory_dump.h>

#include <linux/qpnp/qpnp-adc.h>
#include <soc/qcom/lge/board_lge.h>

#define MODULE_NAME "monitor-thermal"

static struct workqueue_struct *monitor_wq;

struct lge_monitor_thermal_data {
	struct device *dev;
	unsigned int polling_time;
	unsigned int hot_polling_time;
	unsigned int hot_crit_temp;
	unsigned int last_temp;
	struct qpnp_vadc_chip *vadc_dev;
	struct delayed_work init_monitor_work_struct;
	struct delayed_work monitor_work_struct;
};

#ifdef CONFIG_LGE_PM_THERM_VS
struct lge_monitor_thermal_data *vs_therm;
static int64_t vs_temp;
#endif

/*
 * On the kernel command line specify
 * lge_monitor_thermal.enable=1 to enable monitoring thermal node.
 */
static int enable = 1;
module_param(enable, int, 0);

static void poll_monitor_work(struct work_struct *work);
static void init_monitor_work(struct work_struct *work);

static int lge_monitor_thermal_suspend(struct device *dev)
{
return 0;
}

static int lge_monitor_thermal_resume(struct device *dev)
{
return 0;
}

static ssize_t lge_monitor_disable_get(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int ret;
	struct lge_monitor_thermal_data *monitor_dd = dev_get_drvdata(dev);

	ret = snprintf(buf, PAGE_SIZE, "En:%d Poll-time:%d sec\n",
						enable == 0 ? 1 : 0,
						monitor_dd->polling_time);
	return ret;
}

static ssize_t lge_monitor_disable_set(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int ret;
	u8 disable;
	struct lge_monitor_thermal_data *monitor_dd = dev_get_drvdata(dev);

	ret = kstrtou8(buf, 10, &disable);
	if (ret) {
		dev_err(monitor_dd->dev, "invalid user input\n");
		return ret;
	}

	if (enable && disable) {
		/* If have already delayed_work, cancel delayed_work. */
		cancel_delayed_work_sync(&monitor_dd->monitor_work_struct);
	}

	enable = (disable == 1) ? 0 : 1;

	return count;
}

static DEVICE_ATTR(disable, S_IWUSR | S_IRUSR, lge_monitor_disable_get,
						lge_monitor_disable_set);

static void _poll_monitor(struct lge_monitor_thermal_data *monitor_dd)
{
	int rc;
	struct qpnp_vadc_result xo_therm;
#ifdef CONFIG_LGE_PM_BOARD_THERM
	struct qpnp_vadc_result board_therm;
#endif
	monitor_dd->vadc_dev = qpnp_get_vadc(monitor_dd->dev,
						"monitor-thermal");
	if (!IS_ERR(monitor_dd->vadc_dev)) {
		rc = qpnp_vadc_read(monitor_dd->vadc_dev,
					LR_MUX3_PU1_XO_THERM, &xo_therm);
		if (rc)
			pr_err("[XO_THERM] read error with %d\n", rc);
		else {
			pr_info("[XO_THERM] Result:%lld Raw:%d\n",
					xo_therm.physical, xo_therm.adc_code);
		}
#ifdef CONFIG_LGE_PM_BOARD_THERM
		rc = qpnp_vadc_read(monitor_dd->vadc_dev,
					LR_MUX5_AMUX_THM2, &board_therm);
		if (rc)
			pr_err("[BOARD_THERM] read error with %d\n", rc);
		else {
			pr_info("[BOARD_THERM] Result:%lld Raw:%d\n",
					board_therm.physical, board_therm.adc_code);
		}
#endif
#ifdef CONFIG_LGE_PM_THERM_VS
		pr_info("[VS_THERM] Result:%lld \n",vs_temp/100);
#endif
		monitor_dd->last_temp = (unsigned int)xo_therm.physical;
	}
}

static void poll_monitor_work(struct work_struct *work)
{
	unsigned long delay_time;
	struct delayed_work *delayed_work = to_delayed_work(work);
	struct lge_monitor_thermal_data *monitor_dd =
					container_of(delayed_work,
					struct lge_monitor_thermal_data,
					monitor_work_struct);

	if (enable)
		_poll_monitor(monitor_dd);

	/* Recalc polling time as temparature condition */
	if (monitor_dd->last_temp >= monitor_dd->hot_crit_temp)
		delay_time = msecs_to_jiffies(monitor_dd->hot_polling_time);
	else
		delay_time = msecs_to_jiffies(monitor_dd->polling_time);

	/* Check again before scheduling */
	if (enable)
		queue_delayed_work(monitor_wq, &monitor_dd->monitor_work_struct, delay_time);
}

static int lge_monitor_thermal_remove(struct platform_device *pdev)
{
	struct lge_monitor_thermal_data *monitor_dd =
		(struct lge_monitor_thermal_data *)platform_get_drvdata(pdev);

	if (enable)
		cancel_delayed_work_sync(&monitor_dd->monitor_work_struct);

	device_remove_file(monitor_dd->dev, &dev_attr_disable);

	destroy_workqueue(monitor_wq);
	kfree(monitor_dd);
	return 0;
}

static void init_monitor_work(struct work_struct *work)
{
	struct lge_monitor_thermal_data *monitor_dd =
		container_of(to_delayed_work(work),
				struct lge_monitor_thermal_data,
				init_monitor_work_struct);
	unsigned long delay_time;
	int error;

	delay_time = msecs_to_jiffies(monitor_dd->polling_time);

	queue_delayed_work(monitor_wq,
		&monitor_dd->monitor_work_struct, delay_time);

	error = device_create_file(monitor_dd->dev, &dev_attr_disable);
	if (error)
		dev_err(monitor_dd->dev, "cannot create sysfs attribute\n");

	dev_info(monitor_dd->dev, "LGE monitor thermal Initialized\n");

	return;
}

static struct of_device_id lge_monitor_thermal_match_table[] = {
	{ .compatible = "lge,monitor-thermal" },
	{}
};

static int lge_monitor_thermal_dt_to_pdata(struct platform_device *pdev,
					struct lge_monitor_thermal_data *pdata)
{
	struct device_node *node = (pdev) ? pdev->dev.of_node : NULL;
	struct device *dev_mt = (pdev) ? &pdev->dev : NULL;
	int ret;

	if (dev_mt == NULL) {
		dev_err(dev_mt, "failed to get platform_device data\n");
		return -EIO;
	}

	ret = of_property_read_u32(node,
				"lge,hot-poll-time",
				&pdata->hot_polling_time);
	if (ret) {
		dev_err(dev_mt, "reading hot poll time failed\n");
		return -ENXIO;
	}
	ret = of_property_read_u32(node,
				"lge,hot-crit-temp",
				&pdata->hot_crit_temp);
	if (ret) {
		dev_err(dev_mt, "reading hot crit temp failed\n");
		return -ENXIO;
	}
	ret = of_property_read_u32(node,
				"lge,poll-time",
				&pdata->polling_time);
	if (ret) {
		dev_err(dev_mt, "reading poll time failed\n");
		return -ENXIO;
	}

	return 0;
}

static int lge_monitor_thermal_probe(struct platform_device *pdev)
{
	int ret;
	struct lge_monitor_thermal_data *monitor_dd;

	monitor_wq = alloc_workqueue("monitor-thermal", WQ_FREEZABLE, 0);
	if (!monitor_wq) {
		pr_err("Failed to allocate monitor workqueue\n");
		return -EIO;
	}

	if (!pdev->dev.of_node || !enable)
		return -ENODEV;
	monitor_dd = kzalloc(sizeof(struct lge_monitor_thermal_data),
								GFP_KERNEL);
	if (!monitor_dd)
		return -EIO;

	ret = lge_monitor_thermal_dt_to_pdata(pdev, monitor_dd);
	if (ret)
		goto err;

	monitor_dd->dev = &pdev->dev;
	platform_set_drvdata(pdev, monitor_dd);
	INIT_DELAYED_WORK(&monitor_dd->init_monitor_work_struct, init_monitor_work);
	INIT_DELAYED_WORK(&monitor_dd->monitor_work_struct, poll_monitor_work);
	queue_delayed_work(monitor_wq, &monitor_dd->init_monitor_work_struct, 0);
#ifdef CONFIG_LGE_PM_THERM_VS
	vs_therm = monitor_dd;
#endif

	return 0;
err:
	destroy_workqueue(monitor_wq);
	kzfree(monitor_dd);
	return ret;
}

static const struct dev_pm_ops lge_monitor_thermal_dev_pm_ops = {
	.suspend_noirq = lge_monitor_thermal_suspend,
	.resume_noirq = lge_monitor_thermal_resume,
};

#ifdef CONFIG_LGE_PM_THERM_VS
#define VS_COEF 48		/* VS_TEMP_coefficient */

static int get_lge_vs_temp(char *buffer, struct kernel_param *kp)
{
	int rc = 0;

	struct qpnp_vadc_result xo_therm;
	struct qpnp_vadc_result board_therm;

	vs_therm->vadc_dev = qpnp_get_vadc(vs_therm->dev,
						"monitor-thermal");

	if (!IS_ERR(vs_therm->vadc_dev)) {

		rc = qpnp_vadc_read(vs_therm->vadc_dev,
					LR_MUX3_PU1_XO_THERM, &xo_therm);
		if (rc){
			pr_err("[XO_THERM] read error with %d\n", rc);
			return sprintf(buffer, "err");
		}

		rc = qpnp_vadc_read(vs_therm->vadc_dev,
					LR_MUX5_AMUX_THM2, &board_therm);
		if (rc){
			pr_err("[BOARD_THERM] read error with %d\n", rc);
			return sprintf(buffer, "err");
		}

		vs_temp = board_therm.physical*100
			+((xo_therm.physical*10-board_therm.physical))*VS_COEF;

		return sprintf(buffer, "%lld",vs_temp/100);
	}
	return 0;
}
module_param_call(vs_temp, NULL,
			get_lge_vs_temp,&vs_temp, 0644);
#endif

static struct platform_driver lge_monitor_thermal_driver = {
	.probe = lge_monitor_thermal_probe,
	.remove = lge_monitor_thermal_remove,
	.driver = {
		.name = MODULE_NAME,
		.owner = THIS_MODULE,
		.pm = &lge_monitor_thermal_dev_pm_ops,
		.of_match_table = lge_monitor_thermal_match_table,
	},
};

static int init_lge_monitor_thermal(void)
{
	return platform_driver_register(&lge_monitor_thermal_driver);
}

late_initcall(init_lge_monitor_thermal);
MODULE_DESCRIPTION("LGE monitor thermal driver");
MODULE_LICENSE("GPL v2");
