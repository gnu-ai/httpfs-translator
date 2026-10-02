# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

bin_PROGRAMS = httpfs

httpfs_SOURCES = \
    http.c \
    http.h \
    httpfs.c \
    httpfs.h \
    netfs.c

AM_CPPFLAGS = -I$(top_srcdir) $(CURL_CFLAGS)
AM_CFLAGS = $(C23_CFLAGS) -Wall -Wextra

httpfs_LDADD = $(CURL_LIBS) -lnetfs -lihash -lpthread
