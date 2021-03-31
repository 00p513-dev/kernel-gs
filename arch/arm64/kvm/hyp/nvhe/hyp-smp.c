// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 - Google LLC
 * Author: David Brazdil <dbrazdil@google.com>
 */

#include <asm/kvm_asm.h>
#include <asm/kvm_cpufeature.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>

/*
 * Copies of the host's CPU features registers holding sanitized values.
 */
DEFINE_KVM_HYP_CPU_FTR_REG(arm64_ftr_reg_ctrel0);
DEFINE_KVM_HYP_CPU_FTR_REG(arm64_ftr_reg_id_aa64mmfr0_el1);
DEFINE_KVM_HYP_CPU_FTR_REG(arm64_ftr_reg_id_aa64mmfr1_el1);

/*
 * nVHE copy of data structures tracking available CPU cores.
 * Only entries for CPUs that were online at KVM init are populated.
 * Other CPUs should not be allowed to boot because their features were
 * not checked against the finalized system capabilities.
 */
u64 __ro_after_init hyp_cpu_logical_map[NR_CPUS] = { [0 ... NR_CPUS-1] = INVALID_HWID };

u64 cpu_logical_map(unsigned int cpu)
{
	if (cpu >= ARRAY_SIZE(hyp_cpu_logical_map))
		hyp_panic();

	return hyp_cpu_logical_map[cpu];
}

unsigned long __hyp_per_cpu_offset(unsigned int cpu)
{
	unsigned long *cpu_base_array;
	unsigned long this_cpu_base;
	unsigned long elf_base;

	if (cpu >= ARRAY_SIZE(kvm_arm_hyp_percpu_base))
		hyp_panic();

	cpu_base_array = (unsigned long *)&kvm_arm_hyp_percpu_base;
	this_cpu_base = kern_hyp_va(cpu_base_array[cpu]);
	elf_base = (unsigned long)&__per_cpu_start;
	return this_cpu_base - elf_base;
}
