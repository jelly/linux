/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * GNU linker script for the VDSO library.
*
 * Copyright (C) 2012 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 * Heavily based on the vDSO linker scripts for other archs.
 */

#include <linux/const.h>
#include <asm/page.h>
#include <asm/vdso.h>
#include <asm/vdso/vsyscall.h>
#include <asm-generic/vmlinux.lds.h>
#include <vdso/datapage.h>

OUTPUT_FORMAT("elf64-littleaarch64", "elf64-bigaarch64", "elf64-littleaarch64")
OUTPUT_ARCH(aarch64)

SECTIONS
{
	VDSO_VVAR_SYMS

	. = SIZEOF_HEADERS;

	.hash		: { *(.hash) }			:text
	.gnu.hash	: { *(.gnu.hash) }
	.dynsym		: { *(.dynsym) }
	.dynstr		: { *(.dynstr) }
	.gnu.version	: { *(.gnu.version) }
	.gnu.version_d	: { *(.gnu.version_d) }
	.gnu.version_r	: { *(.gnu.version_r) }

	/*
	 * Discard .note.gnu.property sections which are unused and have
	 * different alignment requirement from vDSO note sections.
	 */
	/DISCARD/	: {
		*(.note.GNU-stack .note.gnu.property)
		*(.ARM.attributes)
	}
	.note		: { *(.note.*) }		:text	:note

	. = ALIGN(16);

	.text		: { *(.text*) }			:text	=0xd503201f
	PROVIDE (__etext = .);
	PROVIDE (_etext = .);
	PROVIDE (etext = .);

	. = ALIGN(4);
	.altinstructions : {
		*(.altinstructions)
	}

	.dynamic	: { *(.dynamic) }		:text	:dynamic

	.rela.dyn	: ALIGN(8) { *(.rela .rela*) }

	.rodata		: {
		*(.rodata*)
		*(.got)
		*(.got.plt)
		*(.plt)
		*(.plt.*)
		*(.iplt)
		*(.igot .igot.plt)
	}						:text

	_end = .;
	PROVIDE(end = .);

	DWARF_DEBUG
	ELF_DETAILS

	/DISCARD/	: {
		*(.data .data.* .gnu.linkonce.d.* .sdata*)
		*(.bss .sbss .dynbss .dynsbss)
		*(.eh_frame .eh_frame_hdr)
	}
}

/*
 * We must supply the ELF program headers explicitly to get just one
 * PT_LOAD segment, and set the flags explicitly to make segments read-only.
 */
PHDRS
{
	text		PT_LOAD		FLAGS(5) FILEHDR PHDRS; /* PF_R|PF_X */
	dynamic		PT_DYNAMIC	FLAGS(4);		/* PF_R */
	note		PT_NOTE		FLAGS(4);		/* PF_R */
}

/*
 * This controls what symbols we export from the DSO.
 */
VERSION
{
	LINUX_2.6.39 {
	global:
		__kernel_rt_sigreturn;
		__kernel_gettimeofday;
		__kernel_clock_gettime;
		__kernel_clock_getres;
		__kernel_getrandom;
	local: *;
	};
}

/*
 * Make the sigreturn code visible to the kernel.
 */
VDSO_sigtramp		= __kernel_rt_sigreturn;
