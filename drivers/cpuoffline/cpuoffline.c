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
#include <linux/err.h>

#define MAX_CPU_LEN	8

static int nr_partitions = 0;

static struct cpuoffline_driver *cpuoffline_driver;
DEFINE_MUTEX(cpuoffline_driver_mutex);

static LIST_HEAD(cpuoffline_governor_list);
static DEFINE_MUTEX(cpuoffline_governor_mutex);

static DEFINE_PER_CPU(struct cpuoffline_partition *, cpuoffline_partition);

struct kobject *cpuoffline_global_kobject;
EXPORT_SYMBOL(cpuoffline_global_kobject);

/* sysfs interfaces */

static struct cpuoffline_governor *__find_governor(const char *str_governor)
{
	struct cpuoffline_governor *gov;

	list_for_each_entry(gov, &cpuoffline_governor_list, governor_list)
		if (!strnicmp(str_governor, gov->name, MAX_NAME_LEN))
			return gov;

	return NULL;
}

static ssize_t current_governor_show(struct cpuoffline_partition *partition,
		char *buf)
{
	struct cpuoffline_governor *gov;

	gov = partition->governor;

	if (!gov)
		return 0;

	return snprintf(buf, MAX_NAME_LEN, "%s\n", gov->name);
}

static ssize_t current_governor_store(struct cpuoffline_partition *partition,
		const char *buf, size_t count)
{
	int ret;
	char govstring[MAX_NAME_LEN];
	struct cpuoffline_governor *gov, *tempgov;

	gov = partition->governor;

	ret = sscanf(buf, "%15s", govstring);

	if (ret != 1)
		return -EINVAL;

	tempgov = __find_governor(govstring);

	if (!tempgov)
		return -EINVAL;

	if (!try_module_get(tempgov->owner))
		return -EINVAL;

	/* XXX should gov->stop handle the module put?  probably not */
	if (gov) {
		gov->stop(partition);
		module_put(gov->owner);
	}

	/* XXX kfree the governor? is this a memleak? */
	partition->governor = gov = tempgov;

	gov->start(partition);

	return count;
}

static ssize_t available_governors_show(struct cpuoffline_partition *partition,
		char *buf)
{
	ssize_t ret = 0;
	struct cpuoffline_governor *gov;

	list_for_each_entry(gov, &cpuoffline_governor_list, governor_list)
		ret += snprintf(buf, MAX_NAME_LEN, "%s\n", gov->name);

	return ret;
}

static ssize_t partition_show(struct kobject *kobj, struct attribute *attr,
		char *buf)
{
	struct cpuoffline_partition *partition;
	struct cpuoffline_attribute *c_attr;
	ssize_t ret;

	partition = container_of(kobj, struct cpuoffline_partition, kobj);
	c_attr = container_of(attr, struct cpuoffline_attribute, attr);

	if (!partition || !c_attr)
		return -EINVAL;

	mutex_lock(&partition->mutex);
	/* refcount++ */
	kobject_get(&partition->kobj);

	if (c_attr->show)
		ret = c_attr->show(partition, buf);
	else
		ret = -EIO;

	/* refcount-- */
	kobject_put(&partition->kobj);

	mutex_unlock(&partition->mutex);
	return ret;
}

static ssize_t partition_store(struct kobject *kobj, struct attribute *attr,
		const char *buf, size_t count)
{
	struct cpuoffline_partition *partition;
	struct cpuoffline_attribute *c_attr;
	ssize_t ret = -EINVAL;

	partition = container_of(kobj, struct cpuoffline_partition, kobj);
	c_attr = container_of(attr, struct cpuoffline_attribute, attr);

	if (!partition || !c_attr)
		goto out;

	mutex_lock(&partition->mutex);
	/* refcount++ */
	kobject_get(&partition->kobj);

	if(c_attr->store)
		ret = c_attr->store(partition, buf, count);
	else
		ret = -EIO;

	/* refcount-- */
	kobject_put(&partition->kobj);

out:
	mutex_unlock(&partition->mutex);
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
	char name[MAX_CPU_LEN];
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
	snprintf(name, MAX_CPU_LEN, "cpu%d", sys_dev->id);
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

struct cpuoffline_partition *cpuoffline_partition_init(unsigned int cpu)
{
	int ret = -ENOMEM;
	struct cpuoffline_partition *partition;

	partition = kzalloc(sizeof(struct cpuoffline_partition),
			GFP_KERNEL);
	if (!partition)
		goto out;

	if (!zalloc_cpumask_var(&partition->cpus, GFP_KERNEL))
		goto err_free_partition;

	/* start populating ->cpus with this cpu first */
	cpumask_copy(partition->cpus, cpumask_of(cpu));

	mutex_init(&partition->mutex);

	/* helps sysfs look pretty */
	partition->id = nr_partitions++;

	ret = cpuoffline_driver->init(partition);

	if (ret) {
		pr_err("%s: failed to init driver\n", __func__);
		goto err_free_cpus;
	}

	/* create directory in sysfs for this partition */
	ret = cpuoffline_add_partition_interface(partition);

	/* decrement partition->kobj if the above returns error */
	if (ret) {
		pr_warn("%s: failed to create partition interface\n", __func__);
		kobject_put(&partition->kobj);
	}

	return partition;

err_free_cpus:
	nr_partitions--;
	free_cpumask_var(partition->cpus);
err_free_partition:
	kfree(partition);
out:
	return (void *)ret;
}

/* does not need locking because sequence is synchronous and orderly */
static int cpuoffline_add_dev(struct sys_device *sys_dev)
{
	unsigned int cpu = sys_dev->id;
	int ret = 0;
	struct cpuoffline_partition *partition;

	/* sanity checks */
	if (cpu_is_offline(cpu))
		pr_notice("%s: CPU%d is offline\n", __func__, cpu);

	if (!cpuoffline_driver)
		return -EINVAL;

	partition = per_cpu(cpuoffline_partition, cpu);

	/*
	 * The first cpu in each partition to hit this function will allocate
	 * partition and populate partition's address into the per-cpu data for
	 * each of the CPUs in the same.  It is up to the driver->init function
	 * to do this since only the CPUoffline platform driver knows the
	 * desired topology.
	 *
	 * When the other CPUs in a partition hit this path, their partition
	 * wll have already been allocated.  Only thing left to do is set up
	 * sysfs entries.
	 */
	if (!partition) {
		partition = cpuoffline_partition_init(cpu);

		if (IS_ERR(partition)) {
			pr_warn("%s: failed to create partition\n", __func__);
			return -ENOMEM;
		}
	}

	ret = cpuoffline_add_dev_interface(partition, sys_dev);

	return ret;
}

static int cpuoffline_remove_dev(struct sys_device *sys_dev)
{
	pr_err("%s: GETTING REMOVED!\n", __func__);
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

	pr_info("CPUoffline: registering %s driver", driver->name);

	if (!driver)
		return -EINVAL;

	mutex_lock(&cpuoffline_driver_mutex);

	/* there can only be one */
	if (cpuoffline_driver)
		ret = -EBUSY;
	else
		cpuoffline_driver = driver;

	mutex_unlock(&cpuoffline_driver_mutex);

	if (ret)
		goto out;

	/* register every CPUoffline device */
	ret = sysdev_driver_register(&cpu_sysdev_class,
			&cpuoffline_sysdev_driver);

out:
	return ret;
}
EXPORT_SYMBOL_GPL(cpuoffline_register_driver);

/* FIXME - should this be allowed? */
int cpuoffline_unregister_driver(struct cpuoffline_driver *driver)
{
	pr_info("CPUoffline: unregistering %s driver\n", driver->name);

	return 0;
}
EXPORT_SYMBOL_GPL(cpuoffline_unregister_driver);

/* default driver - single partition containing all CPUs */

#ifdef CONFIG_CPU_OFFLINE_DEFAULT_DRIVER
/**
 * cpuoffline_default_driver_init - create a single partition with all CPUs
 * @partition: CPUoffline partition that is yet to be populated
 *
 * A CPUoffline driver's init function is responsible for two pieces of data.
 * First, for every CPU that should be in @partition, the driver init function
 * must populate a per-cpu pointer to that partition.  Second, for every CPU
 * that should be in @partition, the driver init function must set that bit in
 * the @partition->cpus cpumask.
 */
int cpuoffline_default_driver_init(struct cpuoffline_partition *partition)
{
	unsigned int cpu;

	/* sanity checks */
	if (!partition)
		return -EINVAL;

	cpu = cpumask_first(partition->cpus);

	/* CPU0 should be the only CPU in the mask */
	if (cpu)
		return -EINVAL;

	for_each_possible_cpu(cpu) {
		per_cpu(cpuoffline_partition, cpu) = partition;
		cpumask_set_cpu(cpu, partition->cpus);
	}

	return 0;
}

int cpuoffline_default_driver_exit(struct cpuoffline_partition *partition)
{
	return 0;
}

static struct cpuoffline_driver  cpuoffline_default_driver = {
	.name	= "default",
	.init	= cpuoffline_default_driver_init,
	.exit	= cpuoffline_default_driver_exit,
};

static int __init cpuoffline_register_default_driver(void)
{
	return cpuoffline_register_driver(&cpuoffline_default_driver);
}
late_initcall(cpuoffline_register_default_driver);
#endif

/* CPUoffline governor registration */
int cpuoffline_register_governor(struct cpuoffline_governor *governor)
{
	int ret;

	if (!governor)
		return -EINVAL;

	mutex_lock(&cpuoffline_governor_mutex);

	ret = -EBUSY;
	if (__find_governor(governor->name) == NULL) {
		ret = 0;
		list_add(&governor->governor_list, &cpuoffline_governor_list);
	}

	mutex_unlock(&cpuoffline_governor_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(cpuoffline_register_governor);


/* CPUoffline core initialization */

static int __init cpuoffline_core_init(void)
{
	int cpu;

	pr_info("%s\n", __func__);
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
