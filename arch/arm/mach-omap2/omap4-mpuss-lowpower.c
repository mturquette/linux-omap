/*
 * OMAP4 MPUSS low power code
 *
 * Copyright (C) 2010 Texas Instruments, Inc.
 * Author:
 *      Santosh Shilimkar <santosh.shilimkar@ti.com>
 *
 * OMAP4430 MPUSS mainly consist of dual Cortex-A9 with per-cpu
 * Local timer and Watchdog, GIC, SCU, PL310 L2 cache controller,
 * CPU0 and CPU1 LPRM modules.
 * CPU0, CPU1 and MPUSS have there own power domain and
 * hence multiple low power combinations of MPUSS are possible.
 *
 * The CPU0 and CPU1 can't support Close switch Retention (CSWR)
 * because the mode is not supported by hw constraints of dormant
 * mode. While waking up from the dormant modei,a reset  signal
 * to the Cortex-A9 processor must be asserted by the external
 * power control mechanism
 *
 * With architectural inputs and hardware recommendations, only
 * below modes are supported from power gain vs latency point of view.
 *
 *	CPU0		CPU1		MPUSS
 *	----------------------------------------------
 *	ON(Inactive)	ON(Inactive)	ON(Inactive)
 *	ON(Inactive)	OFF		ON(Inactive)
 *	OFF		OFF		CSWR
 *	OFF		OFF		OSWR
 *	OFF		OFF		OFF
 *	----------------------------------------------
 *
 * Note: CPU0 is the master core and it is the last CPU to go down
 * and first to wake-up when MPUSS low power states are excercised
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/smp.h>
#include <linux/dma-mapping.h>

#include <asm/tlbflush.h>
#include <asm/smp_scu.h>
#include <asm/irq.h>
#include <asm/smp_twd.h>

#include <plat/powerdomain.h>
#include <plat/clockdomain.h>
#include <mach/omap4-common.h>
#include <mach/omap4-wakeupgen.h>

#ifdef CONFIG_SMP
/*
 * CPUx Wakeup Non-Secure Physical Address offsets
 */
#define CPU0_WAKEUP_NS_PA_ADDR_OFFSET		0xa04
#define CPU1_WAKEUP_NS_PA_ADDR_OFFSET		0xa08

/*
 * Scratch pad memory offsets for temorary usages
 */
#define TABLE_ADDRESS_OFFSET			0x01
#define TABLE_VALUE_OFFSET			0x00
#define CR_VALUE_OFFSET				0x02

/*
 * Maximum Secure memory storage size.
 */
#define OMAP4_SECURE_RAM_STORAGE		(88 * SZ_1K)

/*
 * Physical address of secure memory storage
 */
dma_addr_t omap4_secure_ram_phys;

static void *secure_ram;
static struct powerdomain *cpu0_pwrdm, *cpu1_pwrdm, *mpuss_pd;

/*
 * GIC save restore offset from SAR_BANK3
 */
#define SAR_BACKUP_STATUS_OFFSET		0x500
#define SAR_SECURE_RAM_SIZE_OFFSET		0x504
#define SAR_SECRAM_SAVED_AT_OFFSET		0x508
#define ICDISR_CPU0_OFFSET			0x50C
#define ICDISR_CPU1_OFFSET			0x510
#define ICDISR_SPI_OFFSET			0x514
#define ICDISER_CPU0_OFFSET			0x524
#define ICDISER_CPU1_OFFSET			0x528
#define ICDISER_SPI_OFFSET			0x52C
#define ICDIPR_SFI_CPU0_OFFSET			0x53C
#define ICDIPR_PPI_CPU0_OFFSET			0x54C
#define ICDIPR_SFI_CPU1_OFFSET			0x550
#define ICDIPR_PPI_CPU1_OFFSET			0x560
#define ICDIPR_SPI_OFFSET			0x564
#define ICDIPTR_SPI_OFFSET			0x5E4
#define ICDICFR_OFFSET				0x664
#define SAR_BACKUP_STATUS_GIC_CPU0		0x1
#define SAR_BACKUP_STATUS_GIC_CPU1		0x2

/*
 * GIC Save restore bank base
 */
void __iomem *sar_bank3_base;

/*
 * Program the wakeup routine address for the CPU's
 * from OFF/OSWR
 */
static inline void setup_wakeup_routine(unsigned int cpu_id)
{
	if (cpu_id)
		writel(virt_to_phys(omap4_cpu_wakeup_addr()),
				sar_ram_base + CPU1_WAKEUP_NS_PA_ADDR_OFFSET);
	else
		writel(virt_to_phys(omap4_cpu_wakeup_addr()),
				sar_ram_base + CPU0_WAKEUP_NS_PA_ADDR_OFFSET);
}

/*
 * Read CPU's previous power state
 */
static inline unsigned int read_cpu_prev_pwrst(unsigned int cpu_id)
{
	if (cpu_id)
		return pwrdm_read_prev_pwrst(cpu1_pwrdm);
	else
		return pwrdm_read_prev_pwrst(cpu0_pwrdm);
}

/*
 * Clear the CPUx powerdomain's previous power state
 */
static inline void clear_cpu_prev_pwrst(unsigned int cpu_id)
{
	if (cpu_id)
		pwrdm_clear_all_prev_pwrst(cpu1_pwrdm);
	else
		pwrdm_clear_all_prev_pwrst(cpu0_pwrdm);
}

/*
 * Function to restore the table entry that
 * was modified for enabling MMU
 */
static void restore_mmu_table_entry(void)
{
	u32 *scratchpad_address;
	u32 previous_value, control_reg_value;
	u32 *address;

	/*
	 * Get address of entry that was modified
	 */
	scratchpad_address = sar_ram_base + MMU_OFFSET;
	address = (u32 *)readl(scratchpad_address + TABLE_ADDRESS_OFFSET);
	/*
	 * Get the previous value which needs to be restored
	 */
	previous_value = readl(scratchpad_address + TABLE_VALUE_OFFSET);
	address = __va(address);
	*address = previous_value;
	flush_tlb_all();
	control_reg_value = readl(scratchpad_address + CR_VALUE_OFFSET);
	/*
	 * Restore the Control register
	 */
	set_cr(control_reg_value);
}

/*
 * Program the CPU power state using SCU power state register
 */
static void scu_pwrst_prepare(unsigned int cpu_id, unsigned int cpu_state)
{
	u32 scu_pwr_st, regvalue, l1_state;

	switch (cpu_state) {
	case PWRDM_POWER_RET:
		/* DORMANT */
		scu_pwr_st = 0x02;
		l1_state = 0x00;
		break;
	case PWRDM_POWER_OFF:
		scu_pwr_st = 0x03;
		l1_state = 0xff;
		break;
	default:
		/* Not supported */
		return;
	}

	regvalue = readl(scu_base + SCU_CPU_STATUS);
	if (cpu_id)
		regvalue |= scu_pwr_st << 8;
	else
		regvalue |= scu_pwr_st;

	/*
	 * Store the SCU power status value
	 * to scratchpad memory
	 */
	writel(regvalue, sar_ram_base + SCU_OFFSET);
	if (omap_type() != OMAP2_DEVICE_TYPE_GP)
		writel(l1_state, sar_ram_base + SCU_OFFSET + 0x04);
}

/*
 * Save GIC context in SAR RAM. Restore is done by ROM code
 * GIC is lost only when MPU hits OSWR or OFF. It consist
 * of a distributor and a per-cpu interface module
 */
static void save_gic(void)
{
	u32 max_spi_irq, max_spi_reg, reg_index, reg_value;

	/*
	 * GIC needs to be saved in SAR_BANK3
	 */
	sar_bank3_base = sar_ram_base + SAR_BANK3_OFFSET;

	/*
	 * Find out how many interrupts are supported.
	 * The GIC only supports up to 1020 interrupt sources.
	 */
	max_spi_irq = readl(gic_dist_base_addr + GIC_DIST_CTR) & 0x1f;
	max_spi_irq = (max_spi_irq + 1) * 32;
	if (max_spi_irq > max(1020, NR_IRQS))
		max_spi_irq = max(1020, NR_IRQS);
	max_spi_irq = (max_spi_irq - 32);
	max_spi_reg = max_spi_irq / 32;

	/*
	 * Force no Secure Interrupts CPU0 and CPU1
	 */
	writel(0xffffffff, sar_bank3_base + ICDISR_CPU0_OFFSET);
	writel(0xffffffff, sar_bank3_base + ICDISR_CPU1_OFFSET);

	/*
	 * Save all SPI Interrupts secure status
	 */
	for (reg_index = 0; reg_index < max_spi_reg; reg_index++)
		writel(0xffffffff,
			sar_bank3_base + ICDISR_SPI_OFFSET + 4 * reg_index);

	/*
	 * Interrupt Set-Enable and Clear-Enable Registers
	 * Save CPU 0 Enable (set, clear) Interrupts
	 * Force no Interrupts CPU1
	 * Read and Save all SPI Interrupts
	 */
	reg_value = readl(gic_dist_base_addr + GIC_DIST_ENABLE_SET);
	writel(reg_value, sar_bank3_base + ICDISER_CPU0_OFFSET);
	writel(0, sar_bank3_base + ICDISER_CPU1_OFFSET);


	for (reg_index = 0; reg_index < max_spi_reg; reg_index++) {
		reg_value = readl(gic_dist_base_addr +
					0x104 + 4 * reg_index);
		writel(reg_value,
			sar_bank3_base + ICDISER_SPI_OFFSET + 4 * reg_index);
	}

	/*
	 * Interrupt Priority Registers
	 * Secure sw accesses, last 5 bits of the 8 bits (bit[7:3] are used)
	 * Non-Secure sw accesses, last 4 bits (i.e. bits[7:4] are used)
	 * But the Secure Bits[7:3] are shifted by 1 in Non-Secure access.
	 * Secure (bits[7:3] << 1)== Non Secure bits[7:4]
	 *
	 * SGI - backup SGI
	 */
	for (reg_index = 0; reg_index < 4; reg_index++) {
		reg_value = readl(gic_dist_base_addr +
					GIC_DIST_PRI + 4 * reg_index);
		/*
		 * Save the priority bits of the Interrupts
		 */
		writel(reg_value >> 0x1,
		sar_bank3_base + ICDIPR_SFI_CPU0_OFFSET + 4 * reg_index);
		/*
		 * Force the CPU1 interrupt
		 */
		writel(0,
		sar_bank3_base + ICDIPR_SFI_CPU1_OFFSET + 4 * reg_index);
	}
	/*
	 * PPI -  backup PPIs
	 */
	reg_value = readl(gic_dist_base_addr + GIC_DIST_PRI + 0x1c);
	/*
	 * Save the priority bits of the Interrupts
	 */
	writel(reg_value >> 0x1, sar_bank3_base + ICDIPR_PPI_CPU0_OFFSET);
	/*
	 * Force the CPU1 interrupt
	 */
	writel(0, sar_bank3_base + ICDIPR_PPI_CPU1_OFFSET);

	/*
	 * SPI - backup SPI
	 * Interrupt priority regs - 4 interrupts/register
	 */
	for (reg_index = 0; reg_index < (max_spi_irq / 4); reg_index++) {
		reg_value = readl(gic_dist_base_addr +
				(GIC_DIST_PRI + 0x20) + 4 * reg_index);
		writel(reg_value >> 0x1,
			sar_bank3_base + ICDIPR_SPI_OFFSET + 4 * reg_index);
	}

	/*
	 * Interrupt SPI TARGET - 4 interrupts/register
	 */
	for (reg_index = 0; reg_index < (max_spi_irq / 4); reg_index++) {
		reg_value = readl(gic_dist_base_addr +
				(GIC_DIST_TARGET + 0x20) + 4 * reg_index);
		writel(reg_value,
			sar_bank3_base + ICDIPTR_SPI_OFFSET + 4 * reg_index);
	}

	/*
	 * Interrupt SPI Congigeration - 16 interrupts/register
	 */
	for (reg_index = 0; reg_index < (max_spi_irq / 16); reg_index++) {
		reg_value = readl(gic_dist_base_addr +
				(GIC_DIST_CONFIG + 0x08) + 4 * reg_index);
		writel(reg_value,
			sar_bank3_base + ICDICFR_OFFSET + 4 * reg_index);
	}

	/*
	 * Set the Backup Bit Mask status for GIC
	 */
	reg_value = readl(sar_bank3_base + SAR_BACKUP_STATUS_OFFSET);
	reg_value |= (SAR_BACKUP_STATUS_GIC_CPU0 | SAR_BACKUP_STATUS_GIC_CPU1);
	writel(reg_value, sar_bank3_base + SAR_BACKUP_STATUS_OFFSET);
}

/*
 * The CPU interface is per CPU
 */
static inline void enable_gic_cpu_interface(void)
{
	writel(0xf0, gic_cpu_base_addr + GIC_CPU_PRIMASK);
	writel(1, gic_cpu_base_addr + GIC_CPU_CTRL);
}

/*
 * Distributor is enabled by the master CPU
 * Also clear the SAR backup status register
 */
static inline void enable_gic_distributor(void)
{
	writel(0x1, gic_dist_base_addr + GIC_DIST_CTRL);
	if (omap_type() == OMAP2_DEVICE_TYPE_GP)
		writel(0x0, sar_bank3_base + SAR_BACKUP_STATUS_OFFSET);
}

/*
 * API to save GIC and Wakeupgen using secure API
 * for HS/EMU device
 */
static void save_gic_wakeupgen_secure(void)
{
	u32 ret;
	ret = omap4_secure_dispatcher(HAL_SAVEGIC_INDEX,
					FLAG_IRQFIQ_MASK | FLAG_START_CRITICAL,
					0, 0, 0, 0, 0);
	if (!ret)
		pr_debug("GIC and Wakeupgen context save failed\n");
}

/*
 * API to save Secure RAM using secure API
 * for HS/EMU device
 */
static void save_secure_ram(void)
{
	u32 ret;
	ret = omap4_secure_dispatcher(HAL_SAVESECURERAM_INDEX,
					FLAG_IRQFIQ_MASK | FLAG_START_CRITICAL,
					1, omap4_secure_ram_phys, 0, 0, 0);
	if (!ret)
		pr_debug("Secure ram context save failed\n");
}

#ifdef CONFIG_LOCAL_TIMERS

/*
 * Save per-cpu local timer context
 */
static inline void save_local_timers(unsigned int cpu_id)
{
	u32 reg_load, reg_ctrl;

	reg_load = __raw_readl(twd_base + TWD_TIMER_LOAD);
	reg_ctrl = __raw_readl(twd_base + TWD_TIMER_CONTROL);

	if (cpu_id) {
		__raw_writel(reg_load, sar_ram_base + CPU1_TWD_OFFSET);
		__raw_writel(reg_ctrl, sar_ram_base + CPU1_TWD_OFFSET + 0x04);
	} else {
		__raw_writel(reg_load, sar_ram_base + CPU0_TWD_OFFSET);
		__raw_writel(reg_ctrl, sar_ram_base + CPU0_TWD_OFFSET + 0x04);
	}
}

/*
 * restore per-cpu local timer context
 */
static inline void restore_local_timers(unsigned int cpu_id)
{
	u32 reg_load, reg_ctrl;

	if (cpu_id) {
		reg_load = __raw_readl(sar_ram_base + CPU1_TWD_OFFSET);
		reg_ctrl = __raw_readl(sar_ram_base + CPU1_TWD_OFFSET + 0x04);
	} else {
		reg_load = __raw_readl(sar_ram_base + CPU0_TWD_OFFSET);
		reg_ctrl = __raw_readl(sar_ram_base + CPU0_TWD_OFFSET + 0x04);
	}

	__raw_writel(reg_load, twd_base + TWD_TIMER_LOAD);
	__raw_writel(reg_ctrl, twd_base + TWD_TIMER_CONTROL);
}

#else

static void save_local_timers(unsigned int cpu_id)
{}
static void restore_local_timers(unsigned int cpu_id)
{}

#endif

/*
 * OMAP4 MPUSS Low Power Entry Function
 *
 * The purpose of this function is to manage low power programming
 * of OMAP4 MPUSS subsystem
 * Paramenters:
 *	cpu : CPU ID
 *	power_state: Targetted Low power state.
 *
 * MPUSS Low power states
 * The basic rule is that the MPUSS power domain must be at the higher or
 * equal power state (state that consume more power) than the higher of the
 * two CPUs. For example, it is illegal for system power to be OFF, while
 * the power of one or both of the CPU is DORMANT. When an illegal state is
 * entered, then the hardware behavior is unpredictable.
 *
 * MPUSS state for the context save
 * save_state =
 *	0 - Nothing lost and no need to save: MPUSS INACTIVE
 *	1 - CPUx L1 and logic lost: MPUSS CSWR
 *	2 - CPUx L1 and logic lost + GIC lost: MPUSS OSWR
 *	3 - CPUx L1 and logic lost + GIC + L2 lost: MPUSS OFF
 */
void omap4_enter_lowpower(unsigned int cpu, unsigned int power_state)
{
	unsigned int save_state, wakeup_cpu;

	if (cpu > NR_CPUS)
		return;

	/*
	 * Low power state not supported on ES1.0 silicon
	 */
	if (omap_rev() == OMAP4430_REV_ES1_0) {
		wmb();
		do_wfi();
		return;
	}

	switch (power_state) {
	case PWRDM_POWER_ON:
		save_state = 0;
		break;
	case PWRDM_POWER_OFF:
		save_state = 1;
		setup_wakeup_routine(cpu);
		save_local_timers(cpu);
		break;
	case PWRDM_POWER_RET:
		/*
		 * CPUx CSWR is invalid hardware state. Additionally
		 * CPUx OSWR  doesn't give any gain vs CPUxOFF and
		 * hence not supported
		 */
	default:
		/* Invalid state */
		pr_debug("Invalid CPU low power state\n");
		return;
	}

	/*
	 * MPUSS book keeping should be executed by master
	 * CPU only which is the last CPU to go down
	 */
	if (cpu)
		goto cpu_prepare;
	/*
	 * Check MPUSS next state and save GIC if needed
	 * GIC lost during MPU OFF and OSWR
	 */
	switch (pwrdm_read_next_pwrst(mpuss_pd)) {
	case PWRDM_POWER_ON:
		/* No need to save MPUSS context */
		break;
	case PWRDM_POWER_RET:
		/* MPUSS OSWR, logic lost */
		if (pwrdm_read_logic_retst(mpuss_pd) == PWRDM_POWER_OFF) {
			if (omap_type() != OMAP2_DEVICE_TYPE_GP) {
				save_gic_wakeupgen_secure();
			} else {
				save_gic();
				omap4_wakeupgen_save();
			}
			save_state = 2;
		}
		break;
	case PWRDM_POWER_OFF:
		/* MPUSS OFF */
		if (omap_type() != OMAP2_DEVICE_TYPE_GP) {
			save_secure_ram();
			save_gic_wakeupgen_secure();
		} else {
			save_gic();
			omap4_wakeupgen_save();
		}
		save_state = 3;
		break;
	default:
		/* Fall through */
		;
	}

	/*
	 * Program the CPU targeted state
	 */
cpu_prepare:
	clear_cpu_prev_pwrst(cpu);
	pwrdm_clear_all_prev_pwrst(mpuss_pd);
	if (cpu)
		pwrdm_set_next_pwrst(cpu1_pwrdm, power_state);
	else
		pwrdm_set_next_pwrst(cpu0_pwrdm, power_state);
	scu_pwrst_prepare(cpu, power_state);

	/*
	 * Call low level routine to enter to
	 * targeted power state
	 */
	__omap4_cpu_suspend(cpu, save_state);
	wakeup_cpu = hard_smp_processor_id();

	/*
	 * Restore the CPUx power state to ON otherwise CPUx
	 * power domain can transitions to programmed low power
	 * state while doing WFI outside the low powe code. On
	 * secure devices, CPUx does WFI which can result in
	 * domain transition
	 */
	if (wakeup_cpu) {
		pwrdm_set_next_pwrst(cpu1_pwrdm, PWRDM_POWER_ON);
	} else {
		pwrdm_set_next_pwrst(cpu0_pwrdm, PWRDM_POWER_ON);
		pwrdm_set_next_pwrst(mpuss_pd, PWRDM_POWER_ON);
	}

	/*
	 * Check the CPUx previous power state
	 */
	if (read_cpu_prev_pwrst(wakeup_cpu) == PWRDM_POWER_OFF) {
		cpu_init();
		restore_mmu_table_entry();
		restore_local_timers(wakeup_cpu);
	}

	/*
	 * Check MPUSS previous power state and enable
	 * GIC if needed.
	 */
	switch (pwrdm_read_prev_pwrst(mpuss_pd)) {
	case PWRDM_POWER_ON:
		/* No need to restore */
		break;
	case PWRDM_POWER_RET:
		/* FIXME:
		 * if (pwrdm_read_prev_logic_pwrst(mpuss_pd) == PWRDM_POWER_OFF)
		 */
		if (omap_readl(0x4a306324) == PWRDM_POWER_OFF)
			break;
	case PWRDM_POWER_OFF:
		/*
		 * Enable GIC distributor
		 */
		if (!wakeup_cpu)
			enable_gic_distributor();
		/*
		 * Enable GIC cpu inrterface
		 */
		enable_gic_cpu_interface();
		break;
	default:
		;
	}
}

void __init omap4_mpuss_init(void)
{
	struct clockdomain *l4_secure_clkdm;

	cpu0_pwrdm = pwrdm_lookup("cpu0_pwrdm");
	cpu1_pwrdm = pwrdm_lookup("cpu1_pwrdm");
	mpuss_pd = pwrdm_lookup("mpu_pwrdm");
	if (!cpu0_pwrdm || !cpu1_pwrdm || !mpuss_pd)
		pr_err("Failed to get lookup for CPUx/MPUSS pwrdm's\n");

	/*
	 * Check the OMAP type and store it to scratchpad
	 */
	if (omap_type() != OMAP2_DEVICE_TYPE_GP) {
		/* Memory not released */
		secure_ram = dma_alloc_coherent(NULL, OMAP4_SECURE_RAM_STORAGE,
			(dma_addr_t *)&omap4_secure_ram_phys, GFP_KERNEL);
		if (!secure_ram)
			pr_err("Unable to allocate secure ram storage\n");
		writel(0x1, sar_ram_base + OMAP_TYPE_OFFSET);

		/* FIXME: HWSUP isn't working for l4_secure_clkdm */
		l4_secure_clkdm = clkdm_lookup("l4_secure_clkdm");
		omap2_clkdm_wakeup(l4_secure_clkdm);
	} else {
		writel(0x0, sar_ram_base + OMAP_TYPE_OFFSET);
	}

}

#else

void omap4_enter_lowpower(unsigned int cpu, unsigned int power_state)
{
		wmb();
		do_wfi();
		return;
}
void __init omap4_mpuss_init(void)
{
}
#endif
