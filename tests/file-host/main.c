/* URL resolution and local-file loading, checked on the development machine.
 *
 * The case this exists for is the one sentence in the middle of resolve_case
 * below: an absolute reference on a page that came off a card resolves against
 * the card, not against the filesystem root. Get that wrong and a page on an
 * SD card silently reads a disc, or the development host, or whatever else
 * happens to be mounted - which is both a bug and a way out of the mount for
 * anything a page can link to. It is also pure string arithmetic, so there is
 * no excuse for finding out about it on hardware.
 */
#include "ps_file.h"
#include "ps_url.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static int checks;

static void fail(const char *what, const char *got, const char *want)
{
    printf("  FAIL %s\n       got  %s\n       want %s\n", what, got, want);
    failures++;
}

static void eq_str(const char *what, const char *got, const char *want)
{
    checks++;
    if(strcmp(got, want))
        fail(what, got, want);
}

static void eq_int(const char *what, long got, long want)
{
    char g[32], w[32];

    checks++;
    if(got == want)
        return;
    snprintf(g, sizeof g, "%ld", got);
    snprintf(w, sizeof w, "%ld", want);
    fail(what, g, w);
}

/* ------------------------------------------------------------- resolution */

static void resolve_case(const char *base_s, const char *ref, const char *want)
{
    ps_url base, out;
    char   got[PS_URL_MAX];
    char   what[512];

    snprintf(what, sizeof what, "resolve(%s, %s)", base_s, ref);
    checks++;

    if(ps_url_parse(&base, base_s) != 0) {
        fail(what, "<base rejected>", want);
        return;
    }
    if(ps_url_resolve(&out, &base, ref) != 0) {
        fail(what, "<resolve failed>", want);
        return;
    }
    if(ps_url_format(&out, got, sizeof got) < 0) {
        fail(what, "<format failed>", want);
        return;
    }
    if(strcmp(got, want))
        fail(what, got, want);
}

static void parse_case(const char *in, const char *want)
{
    ps_url u;
    char   got[PS_URL_MAX];
    char   what[512];

    snprintf(what, sizeof what, "parse(%s)", in);
    checks++;

    if(ps_url_parse(&u, in) != 0) {
        if(!want)
            return;                 /* expected to be refused */
        fail(what, "<rejected>", want);
        return;
    }
    if(!want) {
        fail(what, "<accepted>", "<rejected>");
        return;
    }
    if(ps_url_format(&u, got, sizeof got) < 0) {
        fail(what, "<format failed>", want);
        return;
    }
    if(strcmp(got, want))
        fail(what, got, want);
}

static void local_path_case(const char *in, const char *want)
{
    ps_url u;
    char   got[PS_URL_PATH_MAX];
    char   what[512];

    snprintf(what, sizeof what, "local_path(%s)", in);
    checks++;

    if(ps_url_parse(&u, in) != 0 ||
       ps_url_local_path(&u, got, sizeof got) != 0) {
        if(!want)
            return;
        fail(what, "<refused>", want);
        return;
    }
    if(!want) {
        fail(what, got, "<refused>");
        return;
    }
    if(strcmp(got, want))
        fail(what, got, want);
}

static void test_urls(void)
{
    puts("url");

    /* A bare absolute path is local media and nothing else it could be. */
    parse_case("/sd/site/index.html", "file:///sd/site/index.html");
    parse_case("/sd", "file:///sd");
    parse_case("file:///cd/test.html", "file:///cd/test.html");
    parse_case("file:///pc/", "file:///pc/");

    /* localhost is the one authority RFC 8089 allows and means this machine;
     * any other names a host there is no protocol here to reach. */
    parse_case("file://localhost/sd/x", "file:///sd/x");
    parse_case("file://server/share/x", NULL);

    /* Typed traversal is clamped at the mount too, not only page-supplied
     * traversal: /sd/.. is /sd and never /cd. */
    parse_case("file:///sd/../cd/secret", "file:///sd/cd/secret");
    parse_case("file:///..", "file:///");
    parse_case("file:///sd/a/../b", "file:///sd/b");
    parse_case("file:///sd/x.html#top", "file:///sd/x.html");

    /* Not a path. A scheme-relative reference needs the base that only
     * ps_url_resolve has, and reading it as "/example.com/x" would open
     * something local under a name that says the opposite. */
    parse_case("//example.com/x", NULL);

    /* The whole point of the task. */
    resolve_case("file:///sd/site/index.html", "img/logo.png",
                 "file:///sd/site/img/logo.png");
    resolve_case("file:///sd/site/index.html", "/img/logo.png",
                 "file:///sd/img/logo.png");
    resolve_case("file:///cd/index.html", "/img/logo.png",
                 "file:///cd/img/logo.png");
    resolve_case("file:///pc/index.html", "/sites.html",
                 "file:///pc/sites.html");
    resolve_case("file:///sd/site/index.html", "../up.png",
                 "file:///sd/up.png");
    resolve_case("file:///sd/site/a/b.html", "../../c.png",
                 "file:///sd/c.png");

    /* A page from the network can carry any of these, so they are untrusted
     * input and must not reach another mount. */
    resolve_case("file:///sd/site/index.html", "../../../../etc/passwd",
                 "file:///sd/etc/passwd");
    resolve_case("file:///sd/site/index.html", "/../cd/x",
                 "file:///sd/cd/x");
    resolve_case("file:///sd/", "../", "file:///sd/");

    /* A directory base keeps its trailing slash, which is what makes a link
     * on a listing resolve inside the directory rather than beside it. */
    resolve_case("file:///sd/a/b/", "c.html", "file:///sd/a/b/c.html");
    resolve_case("file:///sd/a/b/", "../", "file:///sd/a/");
    resolve_case("file:///sd/a/b/", "d/e.png", "file:///sd/a/b/d/e.png");

    /* An absolute URL on a local page still leaves the machine. */
    resolve_case("file:///sd/index.html", "http://example.com/x",
                 "http://example.com/x");

    /* Network resolution is unchanged by any of the above, except that a
     * directory base now keeps its slash there too - which it always should
     * have, and did not. */
    resolve_case("http://h/a/b.html", "c.png", "http://h/a/c.png");
    resolve_case("http://h/a/", "c.png", "http://h/a/c.png");
    resolve_case("http://h/a/b.html", "/c.png", "http://h/c.png");
    resolve_case("http://h/a/b.html", "../../../c", "http://h/c");
    resolve_case("http://h/a/b.html", "//other/x", "http://other/x");
    resolve_case("http://h/a/b.html", "https://s/x", "https://s/x");
    resolve_case("http://h:8080/a/b.html", "c", "http://h:8080/a/c");

    /* What fs_open is finally handed. */
    local_path_case("file:///sd/a%20b.png", "/sd/a b.png");
    local_path_case("file:///sd/img/logo.png?v=2", "/sd/img/logo.png");
    local_path_case("file:///sd/dir/", "/sd/dir");
    local_path_case("file:///", "/");

    /* Decoding after normalisation is where traversal gets back in, so the
     * clamp runs again on the decoded path. */
    local_path_case("file:///sd/%2e%2e/%2e%2e/etc/passwd", "/sd/etc/passwd");
    local_path_case("file:///sd/a%00b", NULL);

    {
        char buf[64];

        checks++;
        if(ps_url_encode_segment("a b#c?d%e", buf, sizeof buf) < 0)
            fail("encode_segment", "<refused>", "a%20b%23c%3Fd%25e");
        else
            eq_str("encode_segment", buf, "a%20b%23c%3Fd%25e");

        checks++;
        if(ps_url_encode_segment("plain-name_1.txt", buf, sizeof buf) < 0)
            fail("encode_segment plain", "<refused>", "plain-name_1.txt");
        else
            eq_str("encode_segment plain", buf, "plain-name_1.txt");

        /* A name that will not fit is refused rather than truncated: half a
         * filename is a link to a different file. */
        eq_int("encode_segment overflow",
               ps_url_encode_segment("aaaaaaaa", buf, 4), -1);
    }

    {
        ps_url u;
        char   root[64];

        checks++;
        if(ps_url_parse(&u, "file:///sd/a/b.html") != 0 ||
           ps_url_local_root(&u, root, sizeof root) != 0)
            fail("local_root", "<refused>", "/sd");
        else
            eq_str("local_root", root, "/sd");

        /* The filesystem root is every mount at once and so is no origin. */
        checks++;
        if(ps_url_parse(&u, "file:///") != 0)
            fail("local_root of /", "<parse failed>", "<refused>");
        else if(ps_url_local_root(&u, root, sizeof root) == 0)
            fail("local_root of /", root, "<refused>");

        checks++;
        if(ps_url_parse(&u, "http://h/a") != 0 || ps_url_is_local(&u))
            fail("is_local(http)", "1", "0");
    }
}

/* ------------------------------------------------------------------ files */

static void write_file(const char *path, const char *body)
{
    FILE *f = fopen(path, "wb");

    if(!f) {
        printf("  FAIL cannot create fixture %s\n", path);
        failures++;
        return;
    }
    fputs(body, f);
    fclose(f);
}

static void rm_rf(const char *path)
{
    DIR           *d = opendir(path);
    struct dirent *e;

    if(d) {
        while((e = readdir(d)) != NULL) {
            char sub[1024];

            if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            rm_rf(sub);
        }
        closedir(d);
        rmdir(path);
        return;
    }
    unlink(path);
}

/* The host filesystem stands in for a card. It is the same code path either
 * way - only probe/read/readdir differ per target - so the listing, the
 * escaping and the size ceiling all get exercised here. */
static void test_files(void)
{
    char cwd[512], root[600], site[700], path[900], url_s[PS_URL_MAX];
    ps_url           u;
    ps_file_response fr;

    puts("file");

    if(!getcwd(cwd, sizeof cwd)) {
        puts("  FAIL getcwd");
        failures++;
        return;
    }

    snprintf(root, sizeof root, "%s/fixture", cwd);
    rm_rf(root);
    mkdir(root, 0755);
    snprintf(site, sizeof site, "%s/site", root);
    mkdir(site, 0755);
    snprintf(path, sizeof path, "%s/img", site);
    mkdir(path, 0755);

    snprintf(path, sizeof path, "%s/index.html", site);
    write_file(path, "<html><body>hi</body></html>");
    snprintf(path, sizeof path, "%s/img/logo.png", site);
    write_file(path, "PNG");
    /* A name that is legal on FAT and is punctuation in a URL. */
    snprintf(path, sizeof path, "%s/a b#c.txt", site);
    write_file(path, "spaced");

    /* A plain read, NUL-terminated the way an HTTP body is - the HTML parser
     * is handed this as a string and cannot tell the two apart. */
    snprintf(url_s, sizeof url_s, "file://%s/index.html", site);
    checks++;
    if(ps_url_parse(&u, url_s) != 0 || ps_file_fetch(&u, &fr) != PS_FILE_OK) {
        fail("fetch index.html", "<failed>", "<ok>");
    }
    else {
        eq_int("index.html length", (long)fr.len, 28);
        eq_str("index.html body", fr.data, "<html><body>hi</body></html>");
        eq_int("index.html is_dir", fr.is_dir, 0);
        ps_file_response_free(&fr);
    }

    snprintf(url_s, sizeof url_s, "file://%s/nope.html", site);
    checks++;
    if(ps_url_parse(&u, url_s) != 0 ||
       ps_file_fetch(&u, &fr) != PS_FILE_ERR_NOT_FOUND)
        fail("fetch missing", "<not 404>", "PS_FILE_ERR_NOT_FOUND");

    /* A percent-encoded name comes back decoded before it reaches the
     * filesystem, which is the other half of the encoding on the listing. */
    snprintf(url_s, sizeof url_s, "file://%s/a%%20b%%23c.txt", site);
    checks++;
    if(ps_url_parse(&u, url_s) != 0 || ps_file_fetch(&u, &fr) != PS_FILE_OK) {
        fail("fetch encoded name", "<failed>", "<ok>");
    }
    else {
        eq_str("encoded name body", fr.data, "spaced");
        ps_file_response_free(&fr);
    }

    /* A directory is a page. */
    snprintf(url_s, sizeof url_s, "file://%s", site);
    checks++;
    if(ps_url_parse(&u, url_s) != 0 || ps_file_fetch(&u, &fr) != PS_FILE_OK) {
        fail("fetch directory", "<failed>", "<ok>");
    }
    else {
        char want_base[PS_URL_MAX];

        eq_int("directory is_dir", fr.is_dir, 1);

        /* Without the trailing slash every link on the page resolves one
         * level too high. */
        snprintf(want_base, sizeof want_base, "file://%s/", site);
        eq_str("directory base", fr.final_url, want_base);

        checks++;
        if(!strstr(fr.data, "href=\"img/\""))
            fail("directory lists img/", "<missing>", "href=\"img/\"");
        checks++;
        if(!strstr(fr.data, "href=\"index.html\""))
            fail("directory lists index.html", "<missing>",
                 "href=\"index.html\"");
        checks++;
        if(!strstr(fr.data, "href=\"a%20b%23c.txt\""))
            fail("directory encodes punctuation", "<missing>",
                 "href=\"a%20b%23c.txt\"");
        checks++;
        if(!strstr(fr.data, "a b#c.txt</a>"))
            fail("directory escapes name text", "<missing>", "a b#c.txt</a>");
        checks++;
        if(!strstr(fr.data, "Up one level"))
            fail("directory offers parent", "<missing>", "Up one level");

        /* The mount's own crumb. Absolute hrefs on a local page are relative
         * to the mount, so the mount is "/" and never its own name repeated. */
        checks++;
        if(!strstr(fr.data, "<a href=\"/\">"))
            fail("directory crumbs to the mount", "<missing>",
                 "<a href=\"/\">");

        /* Directories sort ahead of files. */
        checks++;
        {
            const char *dir  = strstr(fr.data, "href=\"img/\"");
            const char *file = strstr(fr.data, "href=\"index.html\"");

            if(dir && file && dir > file)
                fail("directories sort first", "files first", "dirs first");
        }

        /* And the link actually goes where it looks like it goes: resolving
         * it against the page's own base has to name the real file. */
        {
            ps_url base, ref;
            char   got[PS_URL_MAX], want[PS_URL_MAX];

            checks++;
            snprintf(want, sizeof want, "file://%s/img/logo.png", site);
            if(ps_url_parse(&base, fr.final_url) != 0 ||
               ps_url_resolve(&ref, &base, "img/") != 0)
                fail("listing link resolves", "<failed>", want);
            else {
                ps_url deeper;
                char   deeper_base[PS_URL_MAX];

                if(ps_url_format(&ref, deeper_base, sizeof deeper_base) < 0 ||
                   ps_url_parse(&base, deeper_base) != 0 ||
                   ps_url_resolve(&deeper, &base, "logo.png") != 0 ||
                   ps_url_format(&deeper, got, sizeof got) < 0)
                    fail("listing link resolves", "<failed>", want);
                else
                    eq_str("listing link resolves", got, want);
            }
        }

        ps_file_response_free(&fr);
    }

    /* A cache-buster on a directory URL. Pages get written for the network and
     * then copied onto a disc, so these arrive; left on the base, every link on
     * the generated page would resolve against "dir?v=2" and land nowhere. */
    snprintf(url_s, sizeof url_s, "file://%s?v=2", site);
    checks++;
    if(ps_url_parse(&u, url_s) != 0 || ps_file_fetch(&u, &fr) != PS_FILE_OK) {
        fail("fetch directory with query", "<failed>", "<ok>");
    }
    else {
        char want_base[PS_URL_MAX];

        snprintf(want_base, sizeof want_base, "file://%s/", site);
        eq_str("directory base drops query", fr.final_url, want_base);
        ps_file_response_free(&fr);
    }

    /* An empty directory says so rather than rendering a blank page that
     * looks like a load that failed. */
    snprintf(path, sizeof path, "%s/empty", root);
    mkdir(path, 0755);
    snprintf(url_s, sizeof url_s, "file://%s/", path);
    checks++;
    if(ps_url_parse(&u, url_s) != 0 || ps_file_fetch(&u, &fr) != PS_FILE_OK) {
        fail("fetch empty directory", "<failed>", "<ok>");
    }
    else {
        checks++;
        if(!strstr(fr.data, "Empty"))
            fail("empty directory", "<no marker>", "Empty");
        ps_file_response_free(&fr);
    }

    rm_rf(root);
}

int main(int argc, char **argv)
{
    /* With an argument, print what the browser would be handed for it. A
     * generated listing is a page, and the fastest way to find out whether it
     * lays out is to hand it to tests/layout-host - which wants a file. */
    if(argc > 1) {
        ps_url           u;
        ps_file_response fr;
        ps_file_result   rc;

        if(ps_url_parse(&u, argv[1]) != 0) {
            fprintf(stderr, "not a URL or absolute path: %s\n", argv[1]);
            return 2;
        }
        rc = ps_file_fetch(&u, &fr);
        if(rc != PS_FILE_OK) {
            fprintf(stderr, "%s: %s\n", argv[1], ps_file_strerror(rc));
            return 2;
        }
        fwrite(fr.data, 1, fr.len, stdout);
        ps_file_response_free(&fr);
        return 0;
    }

    test_urls();
    test_files();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
