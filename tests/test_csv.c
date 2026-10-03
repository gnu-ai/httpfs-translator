/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * test_csv.c — Unit tests of the CSV/TSV parser (Phase 3).
 *
 * Covers the RFC 4180 dialect (quotes, doubled quotes, embedded
 * newlines, CRLF), the delimiter sniffing, ragged rows, and both
 * tree orientations (/rows and /columns).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csv.h"
#include "doc.h"

static int failures = 0;

#define CHECK(cond)                                                    \
    do                                                                 \
        {                                                              \
            if (!(cond))                                               \
                {                                                      \
                    printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                            #cond);                                    \
                    failures++;                                         \
                }                                                      \
        }                                                              \
    while (0)

static void check_file (struct doc_node *parent, const char *name,
                        const char *expected)
{
    struct doc_node *f = doc_find (parent, name);

    if (f == NULL)
        {
            printf ("FAIL: no \"%s\" entry\n", name);
            failures++;
            return;
        }

    if (f->data == NULL || f->data_len != strlen (expected)
        || memcmp (f->data, expected, f->data_len) != 0)
        {
            printf ("FAIL: \"%s\": expected \"%s\", got \"%.*s\"\n",
                    name, expected, (int) f->data_len,
                    f->data != NULL ? f->data : "");
            failures++;
        }
}

static void test_basic (void)
{
    static const char doc[] =
        "name,year,license\n"
        "GNU,1983,GPLv3\n"
        "Mach,1985,BSD\n";

    struct doc_node *root = csv_to_doc (doc, strlen (doc), ',', NULL);

    CHECK (root != NULL);
    if (root == NULL)
        return;

    check_file (root, "count", "2\n");

    /* Header: one file per column index, holding the name. */
    struct doc_node *head = doc_find (root, "header");
    CHECK (head != NULL);
    if (head != NULL)
        {
            check_file (head, "0", "name\n");
            check_file (head, "1", "year\n");
            check_file (head, "2", "license\n");
        }

    /* Row view. */
    struct doc_node *rows = doc_find (root, "rows");
    CHECK (rows != NULL);
    if (rows != NULL)
        {
            CHECK (doc_child_count (rows) == 2);

            struct doc_node *r0 = doc_find (rows, "0");
            CHECK (r0 != NULL);
            if (r0 != NULL)
                {
                    check_file (r0, "name", "GNU");
                    check_file (r0, "year", "1983");
                    check_file (r0, "license", "GPLv3");
                }

            check_file (doc_find (rows, "1"), "license", "BSD");
        }

    /* Column view: every value, one per line. */
    struct doc_node *cols = doc_find (root, "columns");
    CHECK (cols != NULL);
    if (cols != NULL)
        {
            check_file (cols, "year", "1983\n1985\n");
            check_file (cols, "name", "GNU\nMach\n");
        }

    doc_release (root);
}

static void test_quoting (void)
{
    static const char doc[] =
        "id,text\n"
        "1,\"hello, world\"\n"
        "2,\"say \"\"hi\"\"\"\n"
        "3,\"line\nbreak\"\r\n"
        "4,plain\r\n";

    struct doc_node *root = csv_to_doc (doc, strlen (doc), ',', NULL);

    CHECK (root != NULL);
    if (root == NULL)
        return;

    check_file (root, "count", "4\n");

    struct doc_node *rows = doc_find (root, "rows");
    CHECK (rows != NULL);
    if (rows != NULL)
        {
            /* A delimiter inside quotes stays in the field. */
            check_file (doc_find (rows, "0"), "text", "hello, world");
            /* A doubled quote is one quote. */
            check_file (doc_find (rows, "1"), "text", "say \"hi\"");
            /* A newline inside quotes stays; CRLF ends records. */
            check_file (doc_find (rows, "2"), "text", "line\nbreak");
            check_file (doc_find (rows, "3"), "text", "plain");
        }

    doc_release (root);
}

static void test_sniffing_and_ragged (void)
{
    /* Semicolon dialect. */
    CHECK (csv_sniff ("a;b;c\n1;2;3\n", 12) == ';');
    /* Tab wins ties. */
    CHECK (csv_sniff ("a\tb\tc\n", 6) == '\t');
    /* Plain comma. */
    CHECK (csv_sniff ("a,b,c\n", 6) == ',');
    /* No delimiter at all: harmless comma default. */
    CHECK (csv_sniff ("abc", 3) == ',');

    /* A ragged row: the missing cell has no file, the surplus
       cell gets a generated name. */
    static const char doc[] =
        "x,y\n"
        "1\n"
        "2,3,4\n";

    struct doc_node *root = csv_to_doc (doc, strlen (doc), ',', NULL);

    CHECK (root != NULL);
    if (root == NULL)
        return;

    struct doc_node *rows = doc_find (root, "rows");
    CHECK (rows != NULL);
    if (rows != NULL)
        {
            struct doc_node *r0 = doc_find (rows, "0");
            CHECK (r0 != NULL);
            if (r0 != NULL)
                {
                    check_file (r0, "x", "1");
                    CHECK (doc_find (r0, "y") == NULL);
                }

            struct doc_node *r1 = doc_find (rows, "1");
            CHECK (r1 != NULL);
            if (r1 != NULL)
                {
                    check_file (r1, "x", "2");
                    check_file (r1, "y", "3");
                    check_file (r1, "col2", "4");
                }
        }

    doc_release (root);
}

static void test_tsv (void)
{
    static const char doc[] =
        "name\tcity\n"
        "Claire\tTouhou\n";

    struct doc_node *root = csv_to_doc (doc, strlen (doc), '\t', NULL);

    CHECK (root != NULL);
    if (root == NULL)
        return;

    check_file (root, "count", "1\n");
    struct doc_node *rows = doc_find (root, "rows");
    CHECK (rows != NULL);
    if (rows != NULL)
        {
            check_file (doc_find (rows, "0"), "name", "Claire");
            check_file (doc_find (rows, "0"), "city", "Touhou");
        }

    doc_release (root);
}

static void test_empty (void)
{
    struct doc_node *root = csv_to_doc ("", 0, ',', NULL);

    CHECK (root != NULL);
    if (root != NULL)
        {
            check_file (root, "count", "0\n");
            doc_release (root);
        }
}

int main (void)
{
    test_basic ();
    test_quoting ();
    test_sniffing_and_ragged ();
    test_tsv ();
    test_empty ();

    if (failures == 0)
        printf ("test_csv: all checks passed\n");
    else
        printf ("test_csv: %d failure(s)\n", failures);

    return failures != 0;
}
