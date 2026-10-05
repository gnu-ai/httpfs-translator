/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * mach-shim.h — Compat with the GNU Mach headers of the
 * 2026-03-14 preinstalled image.
 *
 * The image ships GNU Mach 1.8+git20260224, whose installed
 * headers drop the processor_name_array_t typedef, so the
 * MIG-generated mach_host.h - and with it <mach.h> itself -
 * does not compile.  Every C program including <mach.h>, every
 * translator of this stack included, fails to compile on it.
 *
 * The current ports package (2:1.8+git20260805-4) carries the
 * typedef in <mach/mach_types.h> as
 *     typedef mach_port_t *processor_name_array_t;
 * Define it the SAME way here, BEFORE any Mach or Hurd
 * include: C tolerates the identical redefinition, so this
 * keeps compiling on fixed systems.
 */

#ifndef HTTPFS_MACH_SHIM_H
#define HTTPFS_MACH_SHIM_H

#include <mach/port.h>                  /* mach_port_t */
typedef mach_port_t *processor_name_array_t;

#endif /* HTTPFS_MACH_SHIM_H */
