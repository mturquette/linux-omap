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

#include <linux/mutex.h>
#include <linux/cpuoffline.h>
#include <linux/slab.h>
//#include <linux/kobject.h>
#include <linux/sysfs.h>
//#include <linux/err.h>

#define MAX_NAME_LEN	16

static int nr_partitions = 0;

DEFINE_MUTEX(cpuoffline_mutex);

DEFINE_PER_CPU(struct cpuoffline_partition *, cpuoffline_partition);
DEFINE_PER_CPU(int, cpuoffline_can_offline);

struct cpuoffline_driver *cpuoffline_driver;
struct kobject *cpuoffline_global_kobject;
EXPORT_SYMBOL(cpuoffline_global_kobject);

/* sysfs interfaces */

static ssize_t current_governor_show(struct cpuoffline_partition *partition,
		char *buf)
{
	return snprintf(buf, MAX_NAME_LEN, "%s\n", partition->gov_string);
}

static ssize_t current_governor_store(struct cpuoffline_partition *partition,
		const char *buf, size_t count)
{
	int ret;

	ret = sscanf(buf, "%15s", partition->gov_string);

	if (ret != 1)
		return -EINVAL;

	return count;
}

static ssize_t available_governors_show(struct cpuoffline_partition *partition,
		char *buf)
{
	return snprintf(buf, PAGE_SIZE, "available guvnas\n");
}

static ssize_t partition_show(struct kobject *kobj, struct attribute *attr,
		char *buf)
{
	struct cpuoffline_partition *partition;
	struct cpuoffline_attribute *c_attr;
	ssize_t ret = -EINVAL;

	mutex_lock(&cpuoffline_mutex);
	partition = container_of(kobj, struct cpuoffline_partition, kobj);
	c_attr = container_of(attr, struct cpuoffline_attribute, attr);

	if (!partition || !c_attr)
		goto out;

	/* refcount++ */
	kobject_get(&partition->kobj);

	/* kobject should support per-object show/store function pointers... */
	if (c_attr->show)
		ret = c_attr->show(partition, buf);
	else
		ret = -EIO;

	/* refcount-- */
	kobject_put(&partition->kobj);

out:
	mutex_unlock(&cpuoffline_mutex);
	return ret;
}

static ssize_t partition_store(struct kobject *kobj, struct attribute *attr,
		const char *buf, size_t count)
{
	struct cpuoffline_partition *partition;
	struct cpuoffline_attribute *c_attr;
	ssize_t ret = -EINVAL;

	mutex_lock(&cpuoffline_mutex);
	partition = container_of(kobj, struct cpuoffline_partition, kobj);
	c_attr = container_of(attr, struct cpuoffline_attribute, attr);

	if (!partition || !c_attr)
		goto out;

	/* refcount++ */
	kobject_get(&partition->kobj);

	/* kobject should support per-object show/store function pointers... */
	if(c_attr->store)
		ret = c_attr->store(partition, buf, count);
	else
		ret = -EIO;

	/* refcount-- */
	kobject_put(&partition->kobj);

out:
	mutex_unlock(&cpuoffline_mutex);
	return ret;
}

static struct cpuoffline_attribute current_governor =
	__ATTR(current_governor, (S_IRUGO | S_IWUSR), current_governor_show,
			current_governor_store);

static struct cpuoffline_attribute available_governors =
	__ATTR_RO(available_governors);

static struct attribute *partition_default_attrs[] = {
	&current_governor.attr,
	&available_governors.attr,
	NULL,
};

static const struct sysfs_ops partition_ops = {
	.show	= partition_show,
	.store	= partition_store,
};

static void cpuoffline_partition_release(struct kobject *kobj)
{
	struct cpuoffline_partition *partition;

	partition = container_of(kobj, struct cpuoffline_partition, kobj);

	pr_err("%s: releasing kobj for partition\n", __func__);

	complete(&partition->kobj_unregister);
}

static struct kobj_type partition_ktype = {
	.sysfs_ops	= &partition_ops,
	.default_attrs	= partition_default_attrs,
	.release	= cpuoffline_partition_release,
};

/* cpu class sysdev device registration */

static int cpuoffline_add_dev_interface(struct cpuoffline_partition *partition,
		struct sys_device *sys_dev)
{
	int ret = 0;
	char name[8];
	struct kobject *kobj;

	/* create cpuoffline directory for this CPU */
	/*ret = kobject_init_and_add(&kobj, &ktype_device,
			&sys_dev->kobj, "%s", "cpuoffline");*/
	kobj = kobject_create_and_add("cpuoffline", &sys_dev->kobj);

	if (!kobj) {
		pr_warning("%s: failed to create cpuoffline dir for cpu %d\n",
				__func__, sys_dev->id);
		return -ENOMEM;
	}

#ifdef CONFIG_CPU_OFFLINE_STATISTICS
	/* XXX set up per-CPU statistics here, which is ktype_device */
	/* create directory for cpuoffline stats */
#endif

	/* create a symlink from this cpu to its partition */
	ret = sysfs_create_link(kobj, &partition->kobj, "partition");

	if (ret)
		pr_warning("%s: failed to create symlink from cpu %d to partition %d\n",
				__func__, sys_dev->id, partition->id);

	/* create a symlink from this cpu's partition to itself */
	snprintf(name, 8, "cpu%d", sys_dev->id);
	ret = sysfs_create_link(&partition->kobj, kobj, name);

	if (ret)
		pr_warning("%s: failed to create symlink from partition %d to cpu %d\n",
				__func__, partition->id, sys_dev->id);

	return 0;
}

static int cpuoffline_add_partition_interface(
		struct cpuoffline_partition *partition)
{
	return kobject_init_and_add(&partition->kobj, &partition_ktype,
			cpuoffline_global_kobject, "%s%d", "partition",
			partition->id);
}

static int cpuoffline_add_dev(struct sys_device *sys_dev)
{
	unsigned int cpu = sys_dev->id;
	int ret = 0;
	//char name[MAX_NAME_LEN];
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
	 * The first cpu to hit this path will allocate partition and then
	 * populate that address into the per-cpu data for each cpu in the same
	 * partition via the driver->init function.
	 *
	 * When other CPUs hit this path partition may have already been
	 * allocated.  If so skip both the allocation step as well as the
	 * search for all related cpus.  The only thing left to do for this CPU
	 * is to create per-CPU sysfs entries.
	 */
	if (!partition) {
		pr_err("%s partition is NULL for cpu %d\n", __func__, cpu);

		ret = -ENOMEM;
		partition = kzalloc(sizeof(struct cpuoffline_partition),
				GFP_KERNEL);
		if (!partition)
			goto out;
		pr_err("%s newly alloc'd partition is %p for cpu %d\n",
				__func__, partition, cpu);

		/* start populating ->cpus with this cpu first */
		if (!zalloc_cpumask_var(&partition->cpus, GFP_KERNEL))
			goto err_free_partition;

		cpumask_copy(partition->cpus, cpumask_of(cpu));

		if (!zalloc_cpumask_var(&partition->cpus_can_offline,
					GFP_KERNEL))
			goto err_free_cpus;

		partition->id = nr_partitions++;

		/*
		 * driver->init is responsible for two pieces of data:
		 *
		 * 1) for every CPU in this partition it must populate the
		 * per-cpu *partition pointer, which points to the memory
		 * allocated above
		 *
		 * 2) for every CPU in this partition it must set that bit in
		 * partition->cpus
		 */
		ret = cpuoffline_driver->init(partition);

		if (ret)
			pr_err("%s: handle this error!\n", __func__);

		/* create directory in sysfs for this partition */
		ret = cpuoffline_add_partition_interface(partition);

		if (ret)
			goto err_kobj_partition;

		/* XXX add attribute for current_governor & available_governors */
	} else {
		pr_err("%s partition is initialized to %p for cpu %d\n",
				__func__, partition, cpu);
	}

	/* XXX maybe mutex_unlock here? */

	/* XXX should I try_module_get here? */

	/*
	 * need to set up sysfs interfaces here:
	 */
	/*
	 * kobj refcounting note: every sys_dev added calls kobj_get, including
	 * the first CPU which allocates the partition.  Even if all of the
	 * devices in the partition are removed later on, it will have a
	 * refcount of 1.  The intention is that module unloading should
	 * decrement this final refcount, not sys_dev removal.
	 */
	/*
	 * XXX maybe just pass in partition->kobj next time, or cpu instead of
	 * sys_dev
	 */
	ret = cpuoffline_add_dev_interface(partition, sys_dev);

	goto out;

	//return ret;

err_kobj_partition:
	kobject_put(&partition->kobj);
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

/* driver registration API */

int cpuoffline_register_driver(struct cpuoffline_driver *driver)
{
	int ret = 0;

	pr_info("%s\n", __func__);

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

/* default driver - single partition containing all CPUs */

#ifdef CONFIG_CPU_OFFLINE_DEFAULT_DRIVER
int cpuoffline_default_driver_init(struct cpuoffline_partition *partition)
{
	unsigned int i, cpu;

	pr_err("%s\n", __func__);
	/* sanity checks */
	if (!partition)
		return -EINVAL;

	cpu = cpumask_first(partition->cpus);
	pr_err("%s: cpu is %u\n", __func__, cpu);

	/* CPU0 should be the only CPU in the mask */
	if (cpu)
		return -EINVAL;

	for_each_possible_cpu(i) {
		per_cpu(cpuoffline_partition, i) = partition;
		cpu_set(i, *partition->cpus);
	}

	return 0;
}

int cpuoffline_default_driver_exit(struct cpuoffline_partition *partition)
{
	return 0;
}

static struct cpuoffline_driver  cpuoffline_default_driver = {
	.init	= cpuoffline_default_driver_init,
	.exit	= cpuoffline_default_driver_exit,
};

static int __init cpuoffline_register_default_driver(void)
{
	return cpuoffline_register_driver(&cpuoffline_default_driver);
}
late_initcall(cpuoffline_register_default_driver);
#endif

/* CPUoffline core initialization */

static int __init cpuoffline_core_init(void)
{
	int cpu;

	pr_err("%s\n", __func__);
	for_each_possible_cpu(cpu) {
		per_cpu(cpuoffline_partition, cpu) = NULL;
	}

	cpuoffline_global_kobject = kobject_create_and_add("cpuoffline",
			&cpu_sysdev_class.kset.kobj);

	WARN_ON(!cpuoffline_global_kobject);
	/*register_syscore_ops(&cpuoffline_syscore_ops);*/

	return 0;
}
core_initcall(cpuoffline_core_init);
