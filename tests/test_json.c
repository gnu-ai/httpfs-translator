/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * test_json.c — Unit tests of the JSON parser (Phase 3).
 *
 * The parser is plain POSIX C, so the suite runs on any system:
 * no Hurd, no network, no daemon.  Each case builds a tree from a
 * fixture string and checks names, bytes and error paths — the
 * "environment-agnostic" discipline of PLAN.md Phase 1.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc.h"
#include "json.h"

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

/* check_file — assert that CHILD of PARENT exists, is a file, and
   holds exactly EXPECTED. */
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

static void test_full_document (void)
{
    static const char doc[] =
        "{"
        "\"name\": \"GNU\","
        "\"version\": 0.4,"
        "\"tags\": [\"fs\", \"web\"],"
        "\"nested\": {\"a\": [1, 2, {\"b\": true}]},"
        "\"nothing\": null,"
        "\"empty\": {},"
        "\"esc\": \"line\\nquote\\\" slash\\\\ \\u00e9\""
        "}";

    char *err = NULL;
    struct doc_node *root = json_to_doc (doc, strlen (doc), &err);

    CHECK (root != NULL);
    if (root == NULL)
        {
            printf ("parse error: %s\n", err != NULL ? err : "?");
            free (err);
            return;
        }

    /* An object at the root is re-homed: its members live at the
       top level. */
    check_file (root, "name", "GNU");
    check_file (root, "version", "0.4");
    check_file (root, "nothing", "null");

    /* The \\u00e9 escape decodes to two UTF-8 bytes (0xC3 0xA9). */
    check_file (root, "esc", "line\nquote\" slash\\ \xc3\xa9");

    /* Arrays become directories of numbered entries. */
    struct doc_node *tags = doc_find (root, "tags");
    CHECK (tags != NULL && tags->data == NULL);
    if (tags != NULL)
        {
            check_file (tags, "0", "fs");
            check_file (tags, "1", "web");
            CHECK (doc_child_count (tags) == 2);
        }

    /* Nesting goes as deep as the document says. */
    struct doc_node *n = doc_find (root, "nested");
    CHECK (n != NULL);
    if (n != NULL)
        {
            struct doc_node *a = doc_find (n, "a");
            CHECK (a != NULL);
            if (a != NULL)
                {
                    check_file (a, "0", "1");
                    check_file (a, "1", "2");
                    struct doc_node *two = doc_find (a, "2");
                    CHECK (two != NULL);
                    if (two != NULL)
                        check_file (two, "b", "true");
                }
        }

    /* An empty object stays a (empty) directory. */
    struct doc_node *empty = doc_find (root, "empty");
    CHECK (empty != NULL && empty->data == NULL
           && doc_child_count (empty) == 0);

    doc_release (root);
}

static void test_naming (void)
{
    static const char doc[] =
        "{\"a/b\": 1, \"a_b\": 2, \"a\": 3, \"a\": 4}";

    struct doc_node *root = json_to_doc (doc, strlen (doc), NULL);

    CHECK (root != NULL);
    if (root == NULL)
        return;

    /* "/" cannot live in a file name. */
    check_file (root, "a_b", "1");
    /* The original "a_b" member is uniquified. */
    check_file (root, "a_b_2", "2");
    /* Duplicate keys survive as _2/_3 — the JSON way of exposing
       what a browser would silently overwrite. */
    check_file (root, "a", "3");
    check_file (root, "a_2", "4");

    doc_release (root);
}

static void test_scalar_root (void)
{
    static const char doc42[] = "42";
    static const char doclist[] = "[\"x\", \"y\"]";

    struct doc_node *r1 = json_to_doc (doc42, strlen (doc42), NULL);
    CHECK (r1 != NULL);
    if (r1 != NULL)
        {
            check_file (r1, "value", "42");
            doc_release (r1);
        }

    struct doc_node *r2 = json_to_doc (doclist, strlen (doclist), NULL);
    CHECK (r2 != NULL);
    if (r2 != NULL)
        {
            /* An array at the root is re-homed too: the entries
               live at the top level. */
            check_file (r2, "0", "x");
            check_file (r2, "1", "y");
            doc_release (r2);
        }
}

static void test_errors (void)
{
    static const char *const bad[] =
        {
            "{",                 /* truncated object        */
            "[1,",               /* truncated array         */
            "{\"a\":}",         /* missing value           */
            "{\"a\" 1}",         /* missing colon           */
            "1 2",               /* trailing data           */
            "nul",               /* truncated literal       */
            "\"unterminated",   /* missing closing quote   */
            "{\"a\": 01}",       /* leading zero            */
        };

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        {
            char *err = NULL;
            struct doc_node *r = json_to_doc (bad[i], strlen (bad[i]), &err);

            if (r != NULL)
                {
                    printf ("FAIL: \"%s\" should not parse\n", bad[i]);
                    failures++;
                    doc_release (r);
                }
            free (err);
        }
}

int main (void)
{
    test_full_document ();
    test_naming ();
    test_scalar_root ();
    test_errors ();

    if (failures == 0)
        printf ("test_json: all checks passed\n");
    else
        printf ("test_json: %d failure(s)\n", failures);

    return failures != 0;
}
