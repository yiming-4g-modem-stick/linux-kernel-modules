/* Copyright (c) 2011-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <soc/qcom/spm.h>
#include "spm_driver.h"

#define VDD_DEFAULT 0xDEADF00D

struct msm_spm_power_modes {
	uint32_t mode;
	bool notify_rpm;
	uint32_t start_addr;
};

struct msm_spm_device {
	struct list_head list;
	bool initialized;
	const char *name;
	struct msm_spm_driver_data reg_data;
	struct msm_spm_power_modes *modes;
	uint32_t num_modes;
	uint32_t cpu_vdd;
	struct cpumask mask;
	void __iomem *q2s_reg;
};

struct msm_spm_vdd_info {
	struct msm_spm_device *vctl_dev;
	uint32_t vlevel;
	int err;
};

static LIST_HEAD(spm_list);
static DEFINE_PER_CPU_SHARED_ALIGNED(struct msm_spm_device, msm_cpu_spm_device);
static DEFINE_PER_CPU(struct msm_spm_device *, cpu_vctl_device);

static void msm_spm_smp_set_vdd(void *data)
{
	struct msm_spm_vdd_info *info = (struct msm_spm_vdd_info *)data;
	struct msm_spm_device *dev = info->vctl_dev;

	dev->cpu_vdd = info->vlevel;
	info->err = msm_spm_drv_set_vdd(&dev->reg_data, info->vlevel);
}

/**
 * msm_spm_probe_done(): Verify and return the status of the cpu(s) and l2
 * probe.
 * Return: 0 if all spm devices have been probed, else return -EPROBE_DEFER.
 * if probe failed, then return the err number for that failure.
 */
int msm_spm_probe_done(void)
{
	struct msm_spm_device *dev;
	int cpu;
	int ret = 0;

	for_each_possible_cpu(cpu) {
		dev = per_cpu(cpu_vctl_device, cpu);
		if (!dev)
			return -EPROBE_DEFER;

		ret = IS_ERR(dev);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(msm_spm_probe_done);

void msm_spm_dump_regs(unsigned int cpu)
{
	dump_regs(&per_cpu(msm_cpu_spm_device, cpu).reg_data, cpu);
}

/**
 * msm_spm_set_vdd(): Set core voltage
 * @cpu: core id
 * @vlevel: Encoded PMIC data.
 *
 * Return: 0 on success or -(ERRNO) on failure.
 */
int msm_spm_set_vdd(unsigned int cpu, unsigned int vlevel)
{
	struct msm_spm_vdd_info info;
	struct msm_spm_device *dev = per_cpu(cpu_vctl_device, cpu);
	int ret;

	if (!dev)
		return -EPROBE_DEFER;

	ret = IS_ERR(dev);
	if (ret)
		return ret;

	info.vctl_dev = dev;
	info.vlevel = vlevel;

	ret = smp_call_function_any(&dev->mask, msm_spm_smp_set_vdd, &info,
					true);
	if (ret)
		return ret;

	return info.err;
}
EXPORT_SYMBOL(msm_spm_set_vdd);

/**
 * msm_spm_get_vdd(): Get core voltage
 * @cpu: core id
 * @return: Returns encoded PMIC data.
 */
unsigned int msm_spm_get_vdd(unsigned int cpu)
{
	int ret;
	struct msm_spm_device *dev = per_cpu(cpu_vctl_device, cpu);

	if (!dev)
		return -EPROBE_DEFER;

	ret = IS_ERR(dev);
	if (ret)
		return ret;

	return dev->cpu_vdd;
}
EXPORT_SYMBOL(msm_spm_get_vdd);

static void msm_spm_config_q2s(struct msm_spm_device *dev, unsigned int mode)
{
	uint32_t spm_legacy_mode = 0;
	uint32_t qchannel_ignore = 0;
	uint32_t val = 0;

	if (!dev->q2s_reg)
		return;

	switch (mode) {
	case MSM_SPM_MODE_DISABLED:
	case MSM_SPM_MODE_CLOCK_GATING:
		qchannel_ignore = 1;
		spm_legacy_mode = 0;
		break;
	case MSM_SPM_MODE_RETENTION:
		qchannel_ignore = 0;
		spm_legacy_mode = 0;
		break;
	case MSM_SPM_MODE_GDHS:
	case MSM_SPM_MODE_POWER_COLLAPSE:
		qchannel_ignore = 0;
		spm_legacy_mode = 1;
		break;
	default:
		break;
	}

	val = spm_legacy_mode << 2 | qchannel_ignore << 1;
	__raw_writel(val, dev->q2s_reg);
	mb();
}

static int msm_spm_dev_set_low_power_mode(struct msm_spm_device *dev,
		unsigned int mode, bool notify_rpm)
{
	uint32_t i;
	uint32_t start_addr = 0;
	int ret = -EINVAL;
	bool pc_mode = false;

	if (!dev->initialized)
		return -ENXIO;

	if ((mode == MSM_SPM_MODE_POWER_COLLAPSE)
			|| (mode == MSM_SPM_MODE_GDHS))
		pc_mode = true;

	if (mode == MSM_SPM_MODE_DISABLED) {
		ret = msm_spm_drv_set_spm_enable(&dev->reg_data, false);
	} else if (!msm_spm_drv_set_spm_enable(&dev->reg_data, true)) {
		for (i = 0; i < dev->num_modes; i++) {
			if ((dev->modes[i].mode == mode) &&
				(dev->modes[i].notify_rpm == notify_rpm)) {
				start_addr = dev->modes[i].start_addr;
				break;
			}
		}
		ret = msm_spm_drv_set_low_power_mode(&dev->reg_data,
					start_addr, pc_mode);
	}

	msm_spm_config_q2s(dev, mode);

	return ret;
}

static int msm_spm_dev_init(struct msm_spm_device *dev,
		struct msm_spm_platform_data *data)
{
	int i, ret = -ENOMEM;
	uint32_t offset = 0;

	dev->cpu_vdd = VDD_DEFAULT;
	dev->num_modes = data->num_modes;
	dev->modes = kmalloc(
			sizeof(struct msm_spm_power_modes) * dev->num_modes,
			GFP_KERNEL);

	if (!dev->modes)
		goto spm_failed_malloc;

	dev->reg_data.ver_reg = data->ver_reg;
	ret = msm_spm_drv_init(&dev->reg_data, data);

	if (ret)
		goto spm_failed_init;

	for (i = 0; i < dev->num_modes; i++) {

		/* Default offset is 0 and gets updated as we write more
		 * sequences into SPM
		 */
		dev->modes[i].start_addr = offset;
		ret = msm_spm_drv_write_seq_data(&dev->reg_data,
						data->modes[i].cmd, &offset);
		if (ret < 0)
			goto spm_failed_init;

		dev->modes[i].mode = data->modes[i].mode;
		dev->modes[i].notify_rpm = data->modes[i].notify_rpm;
	}
	msm_spm_drv_flush_seq_entry(&dev->reg_data);
	dev->initialized = true;
	return 0;

spm_failed_init:
	kfree(dev->modes);
spm_failed_malloc:
	return ret;
}

/**
 * msm_spm_turn_on_cpu_rail(): Power on cpu rail before turning on core
 * @base: The SAW VCTL register which would set the voltage up.
 * @val: The value to be set on the rail
 * @cpu: The cpu for this with rail is being powered on
 */
int msm_spm_turn_on_cpu_rail(void __iomem *base, unsigned int val, int cpu)
{
	uint32_t timeout = 2000; /* delay for voltage to settle on the core */
	struct msm_spm_device *dev = per_cpu(cpu_vctl_device, cpu);

	/*
	 * If clock drivers have already set up the voltage,
	 * do not overwrite that value.
	 */
	if (dev && (dev->cpu_vdd != VDD_DEFAULT))
		return 0;

	/* Set the CPU supply regulator voltage */
	val = (val & 0xFF);
	writel_relaxed(val, base);
	mb();
	udelay(timeout);

	/* Enable the CPU supply regulator*/
	val = 0x30080;
	writel_relaxed(val, base);
	mb();
	udelay(timeout);

	return 0;
}
EXPORT_SYMBOL(msm_spm_turn_on_cpu_rail);

void msm_spm_reinit(void)
{
	unsigned int cpu;
	for_each_possible_cpu(cpu)
		msm_spm_drv_reinit(&per_cpu(msm_cpu_spm_device.reg_data, cpu));
}
EXPORT_SYMBOL(msm_spm_reinit);

/*
 * msm_spm_is_mode_avail() - Specifies if a mode is available for the cpu
 * It should only be used to decide a mode before lpm driver is probed.
 * @mode: SPM LPM mode to be selected
 */
bool msm_spm_is_mode_avail(unsigned int mode)
{
	struct msm_spm_device *dev = &__get_cpu_var(msm_cpu_spm_device);
	int i;

	for (i = 0; i < dev->num_modes; i++) {
		if (dev->modes[i].mode == mode)
			return true;
	}

	return false;
}

/**
 * msm_spm_set_low_power_mode() - Configure SPM start address for low power mode
 * @mode: SPM LPM mode to enter
 * @notify_rpm: Notify RPM in this mode
 */
int msm_spm_set_low_power_mode(unsigned int mode, bool notify_rpm)
{
	struct msm_spm_device *dev = &__get_cpu_var(msm_cpu_spm_device);
	return msm_spm_dev_set_low_power_mode(dev, mode, notify_rpm);
}
EXPORT_SYMBOL(msm_spm_set_low_power_mode);

/**
 * msm_spm_init(): Board initalization function
 * @data: platform specific SPM register configuration data
 * @nr_devs: Number of SPM devices being initialized
 */
int __init msm_spm_init(struct msm_spm_platform_data *data, int nr_devs)
{
	unsigned int cpu;
	int ret = 0;

	BUG_ON((nr_devs < num_possible_cpus()) || !data);

	for_each_possible_cpu(cpu) {
		struct msm_spm_device *dev = &per_cpu(msm_cpu_spm_device, cpu);
		ret = msm_spm_dev_init(dev, &data[cpu]);
		if (ret < 0) {
			pr_warn("%s():failed CPU:%u ret:%d\n", __func__,
					cpu, ret);
			break;
		}
	}

	return ret;
}

struct msm_spm_device *msm_spm_get_device_by_name(const char *name)
{
	struct list_head *list;

	list_for_each(list, &spm_list) {
		struct msm_spm_device *dev
			= list_entry(list, typeof(*dev), list);
		if (dev->name && !strcmp(dev->name, name))
			return dev;
	}
	return ERR_PTR(-ENODEV);
}

int msm_spm_config_low_power_mode(struct msm_spm_device *dev,
		unsigned int mode, bool notify_rpm)
{
	return msm_spm_dev_set_low_power_mode(dev, mode, notify_rpm);
}
#ifdef CONFIG_MSM_L2_SPM

/**
 * msm_spm_apcs_set_phase(): Set number of SMPS phases.
 * @cpu: cpu which is requesting the change in number of phases.
 * @phase_cnt: Number of phases to be set active
 */
int msm_spm_apcs_set_phase(int cpu, unsigned int phase_cnt)
{
	struct msm_spm_device *dev = per_cpu(cpu_vctl_device, cpu);

	if (!dev)
		return -ENXIO;

	return msm_spm_drv_set_pmic_data(&dev->reg_data,
			MSM_SPM_PMIC_PHASE_PORT, phase_cnt);
}
EXPORT_SYMBOL(msm_spm_apcs_set_phase);

/** msm_spm_enable_fts_lpm() : Enable FTS to switch to low power
 *                             when the cores are in low power modes
 * @cpu: cpu that is entering low power mode.
 * @mode: The mode configuration for FTS
 */
int msm_spm_enable_fts_lpm(int cpu, uint32_t mode)
{
	struct msm_spm_device *dev = per_cpu(cpu_vctl_device, cpu);

	if (!dev)
		return -ENXIO;

	return msm_spm_drv_set_pmic_data(&dev->reg_data,
			MSM_SPM_PMIC_PFM_PORT, mode);
}
EXPORT_SYMBOL(msm_spm_enable_fts_lpm);

#endif

static int get_cpu_id(struct device_node *node)
{
	struct device_node *cpu_node;
	u32 cpu;
	int ret = -EINVAL;
	char *key = "qcom,cpu";

	cpu_node = of_parse_phandle(node, key, 0);
	if (cpu_node) {
		for_each_possible_cpu(cpu) {
			if (of_get_cpu_node(cpu, NULL) == cpu_node)
				return cpu;
		}
	} else {
		char *key = "qcom,core-id";

		ret = of_property_read_u32(node, key, &cpu);
		if (!ret)
			return cpu;
	}
	return ret;
}

static struct msm_spm_device *msm_spm_get_device(struct platform_device *pdev)
{
	struct msm_spm_device *dev = NULL;
	const char *val = NULL;
	char *key = "qcom,name";
	int cpu = get_cpu_id(pdev->dev.of_node);

	if ((cpu >= 0) && cpu < num_possible_cpus())
		dev = &per_cpu(msm_cpu_spm_device, cpu);
	else if ((cpu == 0xffff) || (cpu < 0))
		dev = devm_kzalloc(&pdev->dev, sizeof(struct msm_spm_device),
					GFP_KERNEL);

	if (!dev)
		return NULL;

	if (of_property_read_string(pdev->dev.of_node, key, &val)) {
		pr_err("%s(): Cannot find a required node key:%s\n",
				__func__, key);
		return NULL;
	}
	dev->name = val;
	list_add(&dev->list, &spm_list);

	return dev;
}

static void get_cpumask(struct device_node *node, struct cpumask *mask)
{
	unsigned long vctl_mask = 0;
	unsigned c = 0;
	int idx = 0;
	struct device_node *cpu_node = NULL;
	int ret = 0;
	char *key = "qcom,cpu-vctl-list";
	bool found = false;

	cpu_node = of_parse_phandle(node, key, idx++);
	while (cpu_node) {
		found = true;
		for_each_possible_cpu(c) {
			if (of_get_cpu_node(c, NULL) == cpu_node)
				cpumask_set_cpu(c, mask);
		}
		cpu_node = of_parse_phandle(node, key, idx++);
	};

	if (found)
		return;

	key = "qcom,cpu-vctl-mask";
	ret = of_property_read_u32(node, key, (u32 *) &vctl_mask);
	if (!ret) {
		for_each_set_bit(c, &vctl_mask, num_possible_cpus()) {
			cpumask_set_cpu(c, mask);
		}
	}
}

static int msm_spm_dev_probe(struct platform_device *pdev)
{
	int ret = 0;
	int cpu = 0;
	int i = 0;
	struct device_node *node = pdev->dev.of_node;
	struct msm_spm_platform_data spm_data;
	char *key = NULL;
	uint32_t val = 0;
	struct msm_spm_seq_entry modes[MSM_SPM_MODE_NR];
	int len = 0;
	struct msm_spm_device *dev = NULL;
	struct resource *res = NULL;
	uint32_t mode_count = 0;

	struct spm_of {
		char *key;
		uint32_t id;
	};

	struct spm_of spm_of_data[] = {
		{"qcom,saw2-cfg", MSM_SPM_REG_SAW2_CFG},
		{"qcom,saw2-avs-ctl", MSM_SPM_REG_SAW2_AVS_CTL},
		{"qcom,saw2-avs-hysteresis", MSM_SPM_REG_SAW2_AVS_HYSTERESIS},
		{"qcom,saw2-avs-limit", MSM_SPM_REG_SAW2_AVS_LIMIT},
		{"qcom,saw2-avs-dly", MSM_SPM_REG_SAW2_AVS_DLY},
		{"qcom,saw2-spm-dly", MSM_SPM_REG_SAW2_SPM_DLY},
		{"qcom,saw2-spm-ctl", MSM_SPM_REG_SAW2_SPM_CTL},
		{"qcom,saw2-pmic-data0", MSM_SPM_REG_SAW2_PMIC_DATA_0},
		{"qcom,saw2-pmic-data1", MSM_SPM_REG_SAW2_PMIC_DATA_1},
		{"qcom,saw2-pmic-data2", MSM_SPM_REG_SAW2_PMIC_DATA_2},
		{"qcom,saw2-pmic-data3", MSM_SPM_REG_SAW2_PMIC_DATA_3},
		{"qcom,saw2-pmic-data4", MSM_SPM_REG_SAW2_PMIC_DATA_4},
		{"qcom,saw2-pmic-data5", MSM_SPM_REG_SAW2_PMIC_DATA_5},
		{"qcom,saw2-pmic-data6", MSM_SPM_REG_SAW2_PMIC_DATA_6},
		{"qcom,saw2-pmic-data7", MSM_SPM_REG_SAW2_PMIC_DATA_7},
	};

	struct mode_of {
		char *key;
		uint32_t id;
		uint32_t notify_rpm;
	};

	struct mode_of mode_of_data[] = {
		{"qcom,saw2-spm-cmd-wfi", MSM_SPM_MODE_CLOCK_GATING, 0},
		{"qcom,saw2-spm-cmd-ret", MSM_SPM_MODE_RETENTION, 0},
		{"qcom,saw2-spm-cmd-gdhs", MSM_SPM_MODE_GDHS, 1},
		{"qcom,saw2-spm-cmd-spc", MSM_SPM_MODE_POWER_COLLAPSE, 0},
		{"qcom,saw2-spm-cmd-pc", MSM_SPM_MODE_POWER_COLLAPSE, 1},
	};

	dev = msm_spm_get_device(pdev);
	if (!dev) {
		ret = -ENOMEM;
		goto fail;
	}
	get_cpumask(node, &dev->mask);

	memset(&spm_data, 0, sizeof(struct msm_spm_platform_data));
	memset(&modes, 0,
		(MSM_SPM_MODE_NR - 2) * sizeof(struct msm_spm_seq_entry));

	key = "qcom,saw2-ver-reg";
	ret = of_property_read_u32(node, key, &val);
	if (ret)
		goto fail;
	spm_data.ver_reg = val;

	key = "qcom,vctl-timeout-us";
	ret = of_property_read_u32(node, key, &val);
	if (!ret)
		spm_data.vctl_timeout_us = val;

	/* SAW start address */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -EFAULT;
		goto fail;
	}

	spm_data.reg_base_addr = devm_ioremap(&pdev->dev, res->start,
					resource_size(res));
	if (!spm_data.reg_base_addr) {
		ret = -ENOMEM;
		goto fail;
	}

	spm_data.vctl_port = -1;
	spm_data.phase_port = -1;
	spm_data.pfm_port = -1;

	key = "qcom,vctl-port";
	of_property_read_u32(node, key, &spm_data.vctl_port);

	key = "qcom,phase-port";
	of_property_read_u32(node, key, &spm_data.phase_port);

	key = "qcom,pfm-port";
	of_property_read_u32(node, key, &spm_data.pfm_port);

	/* Q2S (QChannel-2-SPM) register */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		dev->q2s_reg = devm_ioremap(&pdev->dev, res->start,
						resource_size(res));
		if (!dev->q2s_reg) {
			pr_err("%s(): Unable to iomap Q2S register\n",
					__func__);
			ret = -EADDRNOTAVAIL;
			goto fail;
		}
	}
	/*
	 * At system boot, cpus and or clusters can remain in reset. CCI SPM
	 * will not be triggered unless SPM_LEGACY_MODE bit is set for the
	 * cluster in reset. Initialize q2s registers and set the
	 * SPM_LEGACY_MODE bit.
	 */
	msm_spm_config_q2s(dev, MSM_SPM_MODE_POWER_COLLAPSE);

	for (i = 0; i < ARRAY_SIZE(spm_of_data); i++) {
		ret = of_property_read_u32(node, spm_of_data[i].key, &val);
		if (ret)
			continue;
		spm_data.reg_init_values[spm_of_data[i].id] = val;
	}

	for (i = 0; i < ARRAY_SIZE(mode_of_data); i++) {
		key = mode_of_data[i].key;
		modes[mode_count].cmd =
			(uint8_t *)of_get_property(node, key, &len);
		if (!modes[mode_count].cmd)
			continue;
		modes[mode_count].mode = mode_of_data[i].id;
		modes[mode_count].notify_rpm = mode_of_data[i].notify_rpm;
		pr_debug("%s(): dev: %s cmd:%s, mode:%d rpm:%d\n", __func__,
				dev->name, key, modes[mode_count].mode,
				modes[mode_count].notify_rpm);
		mode_count++;
	}

	spm_data.modes = modes;
	spm_data.num_modes = mode_count;

	ret = msm_spm_dev_init(dev, &spm_data);
	if (ret)
		goto fail;

	platform_set_drvdata(pdev, dev);

	for_each_cpu(cpu, &dev->mask)
		per_cpu(cpu_vctl_device, cpu) = dev;

	return ret;

fail:
	cpu = get_cpu_id(pdev->dev.of_node);
	if (dev && (cpu >= num_possible_cpus() || (cpu < 0))) {
		for_each_cpu(cpu, &dev->mask)
			per_cpu(cpu_vctl_device, cpu) = ERR_PTR(ret);
	}

	pr_err("%s: CPU%d SPM device probe failed: %d\n", __func__, cpu, ret);

	return ret;
}

static int msm_spm_dev_remove(struct platform_device *pdev)
{
	struct msm_spm_device *dev = platform_get_drvdata(pdev);
	list_del(&dev->list);
	return 0;
}

static struct of_device_id msm_spm_match_table[] = {
	{.compatible = "qcom,spm-v2"},
	{},
};

static struct platform_driver msm_spm_device_driver = {
	.probe = msm_spm_dev_probe,
	.remove = msm_spm_dev_remove,
	.driver = {
		.name = "spm-v2",
		.owner = THIS_MODULE,
		.of_match_table = msm_spm_match_table,
	},
};

/**
 * msm_spm_device_init(): Device tree initialization function
 */
int __init msm_spm_device_init(void)
{
	static bool registered;
	if (registered)
		return 0;
	registered = true;
	return platform_driver_register(&msm_spm_device_driver);
}
arch_initcall(msm_spm_device_init);
