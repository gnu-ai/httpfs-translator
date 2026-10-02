# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

AC_INIT([httpfs], [0.2.0], [gcannata23@gmail.com])
AC_CONFIG_SRCDIR([src/httpfs.c])
AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])

AC_CANONICAL_HOST

# System extensions (POSIX/XSI): defines _GNU_SOURCE etc. in config.h.
# Required for loff_t and other interfaces used by libnetfs.  This
# macro must run before anything that compiles.
AC_USE_SYSTEM_EXTENSIONS

AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB

# Le projet est écrit en C23 ; certains compilateurs nomment encore ce
# mode « c2x ».  Exiger l'un des deux.
AC_CACHE_CHECK([whether the C compiler accepts C23], [httpfs_cv_c23], [
  httpfs_cv_c23=no
  for httpfs_flag in -std=c23 -std=c2x; do
    httpfs_save_CFLAGS=$CFLAGS
    CFLAGS="$CFLAGS $httpfs_flag"
    AC_COMPILE_IFELSE(
      [AC_LANG_PROGRAM([[
        constexpr int httpfs_c23_test = 1;
        static void *httpfs_c23_p = nullptr;]],
        [[return httpfs_c23_p == nullptr ? httpfs_c23_test : 0;]])],
      [httpfs_cv_c23=$httpfs_flag])
    CFLAGS=$httpfs_save_CFLAGS
    test "x$httpfs_cv_c23" != xno && break
  done])
AS_IF([test "x$httpfs_cv_c23" = xno],
      [AC_MSG_ERROR([a C23 compiler is required (GCC 13 or newer)])])
AC_SUBST([C23_CFLAGS], [$httpfs_cv_c23])

AC_CONFIG_HEADERS([config.h])

PKG_CHECK_MODULES([CURL], [libcurl >= 7.0.0])

# libmicrohttpd is only needed by the 'make check' test suite; the
# translator itself can be built without it.
PKG_CHECK_MODULES([MHD], [libmicrohttpd >= 0.9.0],
                  [have_mhd=yes], [have_mhd=no])
AM_CONDITIONAL([HAVE_MHD], [test "x$have_mhd" = "xyes"])
AS_IF([test "x$have_mhd" = "xno"],
      [AC_MSG_WARN([libmicrohttpd not found: the test suite will not be built])])

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    tests/Makefile
])
AC_OUTPUT
