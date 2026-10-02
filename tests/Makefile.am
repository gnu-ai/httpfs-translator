# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

# Le drapeau C23 est détecté par configure ; la couche transport
# (src/http.c) est compilée dans le même mode que le traducteur.
AM_CFLAGS = $(C23_CFLAGS) -D_POSIX_C_SOURCE=200112L -Wall -Wextra \
            $(CURL_CFLAGS)

# test_url ne touche pas au réseau : il vérifie la construction
# d'URL (http_url_join) et est donc toujours construit.
check_PROGRAMS = test_url
test_url_SOURCES = test_url.c $(top_srcdir)/src/http.c
test_url_CPPFLAGS = -I$(top_srcdir)/src
test_url_LDADD = $(CURL_LIBS)

if HAVE_MHD

AM_CFLAGS += $(MHD_CFLAGS)

check_LIBRARIES = libtestharness.a
libtestharness_a_SOURCES = test_helpers.c test_helpers.h

check_PROGRAMS += test_full_stream test_range

test_full_stream_SOURCES = test_full_stream.c
test_full_stream_LDADD = libtestharness.a $(MHD_LIBS) $(CURL_LIBS)

test_range_SOURCES = test_range.c
test_range_LDADD = libtestharness.a $(MHD_LIBS) $(CURL_LIBS)

endif

TESTS = $(check_PROGRAMS)
