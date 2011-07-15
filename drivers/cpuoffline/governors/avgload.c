#include <linux/module.h>
#include <linux/cpuoffline.h>

struct cpuoffline_avgload_data {
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_idle;
