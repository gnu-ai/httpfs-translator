/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * test_html.c — Unit tests of the HTML extractor (Phase 3).
 *
 * The fixture is a deliberately messy page: comments, entities,
 * a script with fake links inside, an unterminated anchor, mixed
 * attribute quoting — everything a real-world document throws at
 * a tolerant parser.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc.h"
#include "html.h"

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

/* check_file — the entry NAME of PARENT must exist and hold
   EXPECTED (as a string). */
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

static void test_page (void)
{
    static const char page[] =
        "<!DOCTYPE html>"
        "<html><head>"
        "<title>GNU &amp; Hurd</title>"
        "<meta name=\"description\" content='A &lt;test&gt; page'>"
        "<meta property=\"og:title\" content=\"The OG title\">"
        "<style>a { color: red }</style>"
        "<script>var s = \"</a> not a link\";</script>"
        "</head><body>"
        "<h1>Heading one</h1>"
        "<p>Hello   <b>world</b>!</p>"
        "<h2>Heading &quot;two&quot;</h2>"
        "<a href=\"https://gnu.org\">GNU site</a>"
        "<a HREF='/docs' >Docs</a>"
        "<a href=\"unterminated\">"
        "</body></html>"
        "<!-- a trailing comment with <tags> inside -->";

    struct doc_node *root = html_to_doc (page, strlen (page), NULL);

    CHECK (root != NULL);
    if (root == NULL)
        return;

    /* Entities decode everywhere, and <style>/<script> bodies
       never leak into the text. */
    check_file (root, "title", "GNU & Hurd");

    struct doc_node *text = doc_find (root, "text");
    CHECK (text != NULL);
    if (text != NULL)
        {
            CHECK (strstr (text->data, "Hello world!") != NULL);
            CHECK (strstr (text->data, "color") == NULL);       /* style  */
            CHECK (strstr (text->data, "not a link") == NULL);  /* script */
            CHECK (strstr (text->data, "comment") == NULL);     /* comment */
        }

    /* The meta directory: name= first, property= as fallback. */
    struct doc_node *meta = doc_find (root, "meta");
    CHECK (meta != NULL);
    if (meta != NULL)
        {
            check_file (meta, "description", "A <test> page\n");
            check_file (meta, "og:title", "The OG title\n");
        }

    /* Headings: one numbered file each, "level<TAB>text". */
    struct doc_node *head = doc_find (root, "headings");
    CHECK (head != NULL);
    if (head != NULL)
        {
            check_file (head, "0", "1\tHeading one\n");
            check_file (head, "1", "2\tHeading \"two\"\n");
            CHECK (doc_child_count (head) == 2);
        }

    /* Links: directories with url and text; the unterminated
       anchor is flushed too (tolerance), and nothing inside the
       <script> counts. */
    struct doc_node *links = doc_find (root, "links");
    CHECK (links != NULL);
    if (links != NULL)
        {
            CHECK (doc_child_count (links) == 3);

            struct doc_node *l0 = doc_find (links, "0");
            CHECK (l0 != NULL);
            if (l0 != NULL)
                {
                    check_file (l0, "url", "https://gnu.org\n");
                    check_file (l0, "text", "GNU site\n");
                }

            struct doc_node *l1 = doc_find (links, "1");
            CHECK (l1 != NULL);
            if (l1 != NULL)
                {
                    /* Uppercase attribute names and single quotes. */
                    check_file (l1, "url", "/docs\n");
                    check_file (l1, "text", "Docs\n");
                }

            struct doc_node *l2 = doc_find (links, "2");
            CHECK (l2 != NULL);
            if (l2 != NULL)
                {
                    check_file (l2, "url", "unterminated\n");
                    check_file (l2, "text", "\n");
                }
        }

    doc_release (root);
}

static void test_hostile (void)
{
    /* No HTML at all, garbage, truncation: never crash, always
       produce a (mostly empty) tree. */
    static const char *const inputs[] =
        {
            "",
            "just text",
            "<<>>",
            "<a href=\"x",                /* truncated attribute */
            "<title>never closed",
            "<script>never closed either",
            "<h1>heading never closed",
        };

    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++)
        {
            struct doc_node *r =
                html_to_doc (inputs[i], strlen (inputs[i]), NULL);
            CHECK (r != NULL);
            doc_release (r);
        }
}

int main (void)
{
    test_page ();
    test_hostile ();

    if (failures == 0)
        printf ("test_html: all checks passed\n");
    else
        printf ("test_html: %d failure(s)\n", failures);

    return failures != 0;
}
