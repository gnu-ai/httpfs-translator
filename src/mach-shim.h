/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * mach-shim.h — Compat with the forky/sid GNU Mach snapshot.
 *
 * The Debian GNU/Hurd 2026-03-14 image ships GNU Mach
 * 1.8+git20260224, whose installed mach_host.h uses
 * processor_name_array_t without any installed header defining
 * it (the MIG header generation of this snapshot drops the
 * typedef).  Every C program including <mach.h> — every
 * translator of this stack included — fails to compile on it.
 *
 * Include this header BEFORE any Mach or Hurd include; it
 * provides the canonical definition (an array of
 * processor_info_t).  C tolerates the identical redefinition the
 * day the snapshot is fixed.
 */

#ifndef HTTPFS_MACH_SHIM_H
#define HTTPFS_MACH_SHIM_H

#include <mach/processor_info.h>
typedef processor_info_t *processor_name_array_t;

#endif /* HTTPFS_MACH_SHIM_H */
