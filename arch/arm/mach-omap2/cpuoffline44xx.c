#include <linux/cpu.h>
#include <linux/cpuoffline.h>

/* XXX move this to plat-omap/cpu-omap.c */

/**
 * omap_cpuoffline_driver_init - populate a partition with all affected CPUs
 * @partition: the partition to which affected CPUs belong.  Should be
 * pre-populated with exactly one CPU.
 *
 * Partitions are pre-defined on OMAP4 since we know the topology already.  We
 * have only 1 partition with both CPUs in it.  Due to static CPU mapping we
 * also assume that CPU0 (master) is the first one populated in this partition.
 * To keep this somewhat generic-looking (for future OMAP silicon) iterate over
 * each possbile CPU and do the following:
 * 1) put it in the parition since we want *all* CPUs in a single partition
 * 2) mark every CPU *except* for CPU0 as can_offline
 */
static int omap_cpuoffline_driver_init(struct cpuoffline_partition *partition)
{
	unsigned int cpu, i;
	int ret = -EINVAL;

	pr_err("%s\n", __func__);
	/* sanity checks */
	if (!partition)
		goto out;

	cpu = cpumask_first(partition->cpus);
	pr_err("%s: cpu is %u\n", __func__, cpu);

	/* CPU0 should be the only CPU in the mask */
	if (cpu)
		goto out;

	/*
	 * For OMAP4 we want only a single partition for all CPUs.  Also we do
	 * not want CPU0 to be taken offline by the CPUoffline framework.  All
	 * other CPUs managed through the framework can go offline (in the
	 * OMAP4 case this means CPU1).
	 *
	 * Populating these settings is done below in a procedural fashion by
	 * looping over all possible CPUs, but it could also be done by a
	 * look-up table which knows the static mapping of CPUs and their
	 * offlining characteristics.
	 *
	 * For architectures that have multiple CPUoffline partitions, looping
	 * over all possible CPUs is a bad idea.  Instead a static cpumask
	 * containing the per-partition CPU mapping should be used in
	 * combination with a for_each_cpu loop.
	 */

	/*
	 * XXX design note: haven't decided whether to keep all the per-cpu
	 * data or just manage it centrally in parition
	 */
	for_each_possible_cpu(i) {
		per_cpu(cpuoffline_partition, i) = partition;
		cpu_set(i, *partition->cpus);

		if (cpu != i) {
			per_cpu(cpuoffline_can_offline, i) = 1;
			/* cpumask_or(partition->cpus, */
			cpu_set(i, *partition->cpus_can_offline);
		}
	}

out:
	return 0;
}

static int omap_cpuoffline_driver_exit(struct cpuoffline_partition *partition)
{
	return 0;
}

static struct cpuoffline_driver  omap_cpuoffline_driver = {
	.init	= omap_cpuoffline_driver_init,
	.exit	= omap_cpuoffline_driver_exit,
};

static int __init omap_cpuoffline_init(void)
{
	return cpuoffline_register_driver(&omap_cpuoffline_driver);
}
late_initcall(omap_cpuoffline_init);
