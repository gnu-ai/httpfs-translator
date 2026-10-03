# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

bin_PROGRAMS = httpfs htmlfs jsonfs csvfs tsvfs

# --- Phase 1-2: the transport translator -----------------------------

httpfs_SOURCES = \
    http.c \
    http.h \
    httpfs.c \
    httpfs.h \
    netfs.c

# --- Phase 3: the parser chain ----------------------------------------
#
# Every parser turns a document into the portable doc tree
# (doc.c); parserfs.c is the one libnetfs server shared by the
# four parser translators; csvfs and tsvfs are the same program
# (csvfs.c sniffs its name to pick the default delimiter).

PARSERFS_SOURCES = \
    doc.c \
    doc.h \
    parserfs.c \
    parserfs.h

htmlfs_SOURCES = $(PARSERFS_SOURCES) html.c html.h htmlfs.c
jsonfs_SOURCES = $(PARSERFS_SOURCES) json.c json.h jsonfs.c
csvfs_SOURCES  = $(PARSERFS_SOURCES) csv.c csv.h csvfs.c
tsvfs_SOURCES  = $(PARSERFS_SOURCES) csv.c csv.h csvfs.c

AM_CPPFLAGS = -I$(top_srcdir) $(CURL_CFLAGS)
AM_CFLAGS = $(C23_CFLAGS) -Wall -Wextra

# The transport needs libcurl; the parsers need nothing but the
# tree and the C library (the zero-dependency policy of
# CONTEXT.md), and the server needs the usual Hurd libraries.
httpfs_LDADD = $(CURL_LIBS) -lnetfs -lihash -liohelp -lmachuser -lpthread

PARSERFS_LDADD = -lnetfs -lihash -liohelp -lmachuser -lpthread
htmlfs_LDADD = $(PARSERFS_LDADD)
jsonfs_LDADD = $(PARSERFS_LDADD)
csvfs_LDADD = $(PARSERFS_LDADD)
tsvfs_LDADD = $(PARSERFS_LDADD)
