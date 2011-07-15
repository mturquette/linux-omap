/*
 * CPU Offline framework core
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Mike Turquette <mturquette@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

//#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/cpuoffline.h>
#include <linux/slab.h>

DEFINE_MUTEX(cpuoffline_mutex);

static DEFINE_PER_CPU(struct cpuoffline_partition *, cpuoffline_partition);
static DEFINE_PER_CPU(int, cpuoffline_can_offline);

struct cpuoffline_driver *cpuoffline_driver;
struct kobject *cpuoffline_global_kobject;
EXPORT_SYMBOL(cpuoffline_global_kobject);

/* XXX no cpuoffline_syscore_ops for now */

static int cpuoffline_add_dev(struct sys_device *sys_dev)
{
	unsigned int cpu = sys_dev->id;
	int ret = 0;
	struct cpuoffline_partition *partition;

	pr_err("%s: cpu is %d\n", __func__, cpu);

	/* sanity checks */
	if (cpu_is_offline(cpu))
		pr_err("%s: cpu %d is offline\n", __func__, cpu);

	if (!cpuoffline_driver) {
		pr_err("%s: no cpuoffline driver registered\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	/* XXX should I try_module_get here? */

	mutex_lock(&cpuoffline_mutex);
	partition = per_cpu(cpuoffline_partition, cpu);

	/*
	 * The first cpu to hit this path will malloc partition and then
	 * populate that address into the per-cpu data for each cpu in the same
	 * partition via the driver->init function.
	 *
	 * Thus partition may have already been populated by another cpu.  If
	 * so skip that allocation step as well as the search for all related
	 * cpus.  In such a case the only remaining step for this cpu is to
	 * create per-cpu sysfs entries.
	 */
	if (!partition) {
		pr_err("%s partition is NULL for cpu %d\n", __func__, cpu);

		/* XXX copy/paste: review for correctness! */
		ret = -ENOMEM;
		partition = kzalloc(sizeof(struct cpuoffline_partition),
				GFP_KERNEL);
		if (!partition)
			goto out;

		/* start populating ->cpus with this cpu first */
		if (!zalloc_cpumask_var(&partition->cpus, GFP_KERNEL))
			goto err_free_partition;

		cpumask_copy(partition->cpus, cpumask_of(cpu));

		if (!zalloc_cpumask_var(&partition->cpus_can_offline,
					GFP_KERNEL))
			goto err_free_cpus;

		/*
		 * XXX random thought: I hope that Linux allows me to "share"
		 * the cpuoffline_partition pointer across CPUs!  I'm sort of
		 * making the assumption that it is OK for each CPU to know
		 * about cpuoffline_parition *partition, which may not be
		 * true...
		 */

		/*
		 * XXX pick up here tomorrow at line 939 in cpufreq.c
		 * don't forget to read the XXX note below!
		 */

		/*
		 * The driver->init is responsible for populating two pieces of
		 * data:
		 * 1) for every CPU in this partition it must populate the
		 * per-cpu *partition pointer, which points to the memory
		 * allocated above
		 * 2) for every CPU in this partition it must set that bit in
		 * partition->cpus
		 */
		ret = cpuoffline_driver->init(partition);

		/*
		 * XXX two options here:
		 * 1) cpuoffline_driver->init can set *every* CPU's partition
		 * pointer to the one passed in, including *this* CPUs pointer.
		 * OR,
		 * 2) this code can run:
		 * per_cpu(cpuoffline_partition, cpu) = partition;
		 * first and then call cpuoffline_driver->init
		 */
	} else {
		pr_err("%s partition is initialized for cpu %d\n", __func__, cpu);
	}

	mutex_unlock(&cpuoffline_mutex);
	goto out;

	/* XXX should I try_module_get here? */

	/*ret = cpufreq_add_dev_policy(cpu, policy, sys_dev);
	ret = cpufreq_add_dev_interface(cpu, policy, sys_dev);*/

	return ret;

err_free_cpus:
	free_cpumask_var(partition->cpus);
err_free_partition:
	kfree(partition);
out:
	mutex_unlock(&cpuoffline_mutex);
	return ret;
}

static int cpuoffline_remove_dev(struct sys_device *sys_dev)
{
	return 0;
}

static struct sysdev_driver cpuoffline_sysdev_driver = {
	.add	= cpuoffline_add_dev,
	.remove	= cpuoffline_remove_dev,
};

int cpuoffline_register_driver(struct cpuoffline_driver *driver)
{
	int ret = 0;

	pr_err("%s\n", __func__);

	if (!driver)
		return -EINVAL;

	mutex_lock(&cpuoffline_mutex);

	/* there can only be one */
	if (cpuoffline_driver)
		ret = -EBUSY;
	else
		cpuoffline_driver = driver;

	mutex_unlock(&cpuoffline_mutex);

	if (ret)
		goto out;

	/* register every CPUoffline device */
	ret = sysdev_driver_register(&cpu_sysdev_class,
			&cpuoffline_sysdev_driver);

out:
	return ret;
}
EXPORT_SYMBOL_GPL(cpuoffline_register_driver);

int cpuoffline_unregister_driver(struct cpuoffline_driver *driver)
{
	return 0;
}
EXPORT_SYMBOL_GPL(cpuoffline_unregister_driver);

static int __init cpuoffline_core_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		per_cpu(cpuoffline_partition, cpu) = NULL;
	}

	cpuoffline_global_kobject = kobject_create_and_add("cpuoffline",
			&cpu_sysdev_class.kset.kobj);

	BUG_ON(!cpuoffline_global_kobject);
	/*register_syscore_ops(&cpuoffline_syscore_ops);*/

	return 0;
}
core_initcall(cpuoffline_core_init);
