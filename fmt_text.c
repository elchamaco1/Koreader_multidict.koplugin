/* Formatos de texto sin índice propio: DSL (ABBYY Lingvo), XDXF y TSV/tab.
 *
 * La primera vez se recorre el archivo y se genera un índice auxiliar .mdidx
 * (ver sidx.c). Las búsquedas posteriores leen solo la entrada pedida, en
 * acceso aleatorio, también dentro de .dsl.dz (dictzip). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "md_internal.h"

typedef struct {
    rfile *rf;
    sidx *idx;
    int enc;
    int kind; /* 0 dsl, 1 xdxf, 2 tab, 3 stardict textual, 4 csv */
} textdict;

typedef int (*build_fn)(rfile *rf, sidx_builder *b, char **name, char **lf, char **lt, char *err, int errlen);
typedef void (*render_fn)(const char *entry, size_t n, const char *word, mbuf *out);

/* ====================================================================== */
/* DSL                                                                     */
/* ====================================================================== */

static char *dsl_header_value(const char *line)
{
    const char *q1 = strchr(line, '"');
    if (!q1) return NULL;
    const char *q2 = strrchr(q1 + 1, '"');
    if (!q2 || q2 <= q1) return NULL;
    return md_strndup(q1 + 1, (size_t)(q2 - q1 - 1));
}

/* Genera las variantes de búsqueda de un encabezado DSL:
 *  - lo que está entre llaves {…} no forma parte de la clave
 *  - lo que está entre paréntesis sin escapar es opcional: colo(u)r -> color, colour
 *  - "\x" es el carácter x literal */
static void dsl_variants(const char *hw, size_t n, mbuf *full, mbuf *shrt)
{
    int brace = 0, paren = 0;
    for (size_t i = 0; i < n; i++) {
        char c = hw[i];
        if (c == '\\' && i + 1 < n) {
            i++;
            if (!brace) {
                mb_putc(full, hw[i]);
                if (!paren) mb_putc(shrt, hw[i]);
            }
            continue;
        }
        if (c == '{') { brace++; continue; }
        if (c == '}') { if (brace) brace--; continue; }
        if (brace) continue;
        if (c == '(') { paren++; continue; }
        if (c == ')') { if (paren) paren--; continue; }
        mb_putc(full, c);
        if (!paren) mb_putc(shrt, c);
    }
    mb_cstr(full);
    mb_cstr(shrt);
}

typedef struct {
    char **hw;
    size_t n, cap;
} strlist;

static void sl_add(strlist *l, const char *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->hw = realloc(l->hw, l->cap * sizeof(char *));
    }
    l->hw[l->n++] = md_strdup(s);
}
static void sl_clear(strlist *l)
{
    for (size_t i = 0; i < l->n; i++) free(l->hw[i]);
    l->n = 0;
}

static void dsl_flush(sidx_builder *b, strlist *hws, uint64_t start, uint64_t end)
{
    mbuf full = {0}, shrt = {0};
    for (size_t i = 0; i < hws->n; i++) {
        full.n = shrt.n = 0;
        dsl_variants(hws->hw[i], strlen(hws->hw[i]), &full, &shrt);
        sidx_add(b, full.p, full.n, start, (uint32_t)(end - start));
        if (shrt.n != full.n || memcmp(shrt.p, full.p, full.n))
            sidx_add(b, shrt.p, shrt.n, start, (uint32_t)(end - start));
    }
    mb_free(&full);
    mb_free(&shrt);
    sl_clear(hws);
}

static int dsl_build(rfile *rf, sidx_builder *b, char **name, char **lf, char **lt, char *err, int errlen)
{
    (void)err; (void)errlen;
    linereader lr;
    lr_init(&lr, rf);
    mbuf line = {0};
    uint64_t s, e, entry_start = 0;
    strlist hws = {0};
    int in_body = 0, in_header = 1;
    while (lr_next(&lr, &line, &s, &e)) {
        const char *l = line.p;
        if (in_header && l[0] == '#') {
            if (!strncmp(l, "#NAME", 5)) { free(*name); *name = dsl_header_value(l); }
            else if (!strncmp(l, "#INDEX_LANGUAGE", 15)) { free(*lf); *lf = dsl_header_value(l); }
            else if (!strncmp(l, "#CONTENTS_LANGUAGE", 18)) { free(*lt); *lt = dsl_header_value(l); }
            continue;
        }
        if (line.n == 0) continue;
        int is_body = (l[0] == ' ' || l[0] == '\t');
        if (!is_body && l[0] == '#' && in_header) continue;
        in_header = 0;
        if (is_body) {
            if (hws.n) in_body = 1;
            continue;
        }
        /* línea de encabezado */
        if (in_body) {
            dsl_flush(b, &hws, entry_start, s);
            in_body = 0;
        }
        if (hws.n == 0) entry_start = s;
        sl_add(&hws, l);
    }
    if (hws.n) dsl_flush(b, &hws, entry_start, lr.raw_off + lr.raw.n);
    free(hws.hw);
    mb_free(&line);
    lr_free(&lr);
    return 0;
}

static void put_link(mbuf *out, const char *t, size_t n)
{
    mb_puts(out, "<a href=\"");
    mb_html_escape(out, t, n);
    mb_puts(out, "\">");
    mb_html_escape(out, t, n);
    mb_puts(out, "</a>");
}

/* Convierte una línea de cuerpo DSL a HTML. */
static void dsl_line_html(const char *s, size_t n, const char *hw, mbuf *out)
{
    int open_m = 0;
    if (n && s[0] == '@') { /* subentrada */
        s++; n--;
        while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
        mb_puts(out, "<div><b>");
        mb_html_escape(out, s, n);
        mb_puts(out, "</b></div>");
        return;
    }
    int has_m = (n >= 3 && s[0] == '[' && s[1] == 'm');
    if (!has_m) mb_puts(out, "<div>");
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) { mb_html_escape(out, s + i + 1, 1); i += 2; continue; }
        if (c == '{' && i + 1 < n && s[i + 1] == '{') {
            const char *endc = strstr(s + i + 2, "}}");
            i = endc && (size_t)(endc - s) < n ? (size_t)(endc - s) + 2 : n;
            continue;
        }
        if (c == '<' && i + 1 < n && s[i + 1] == '<') {
            const char *endl = strstr(s + i + 2, ">>");
            if (endl && (size_t)(endl - s) < n) {
                put_link(out, s + i + 2, (size_t)(endl - (s + i + 2)));
                i = (size_t)(endl - s) + 2;
                continue;
            }
        }
        if (c == '~') { mb_html_escape(out, hw, strlen(hw)); i++; continue; }
        if (c != '[') { mb_html_escape(out, s + i, 1); i++; continue; }

        /* etiqueta */
        size_t j = i + 1;
        while (j < n && s[j] != ']') j++;
        if (j >= n) { mb_html_escape(out, s + i, n - i); break; }
        char tag[64];
        size_t tl = j - i - 1;
        if (tl >= sizeof tag) tl = sizeof tag - 1;
        memcpy(tag, s + i + 1, tl);
        tag[tl] = 0;
        i = j + 1;
        int close = tag[0] == '/';
        char *t = tag + close;
        char *arg = strchr(t, ' ');
        if (arg) *arg++ = 0;

        if (t[0] == 'm' && (t[1] == 0 || isdigit((unsigned char)t[1]))) {
            if (close) { if (open_m) { mb_puts(out, "</div>"); open_m--; } }
            else { mb_printf(out, "<div style=\"margin-left:%dem\">", t[1] ? t[1] - '0' : 0); open_m++; }
        } else if (!strcmp(t, "b") || !strcmp(t, "i") || !strcmp(t, "u") || !strcmp(t, "sup") || !strcmp(t, "sub")) {
            mb_printf(out, close ? "</%s>" : "<%s>", t);
        } else if (!strcmp(t, "c")) {
            if (close) mb_puts(out, "</span>");
            else mb_printf(out, "<span style=\"color:%s\">", arg && *arg ? arg : "green");
        } else if (!strcmp(t, "p")) {
            mb_puts(out, close ? "</i>" : "<i style=\"color:green\">");
        } else if (!strcmp(t, "ex")) {
            mb_puts(out, close ? "</span>" : "<span style=\"color:#555\">");
        } else if (!strcmp(t, "com")) {
            mb_puts(out, close ? "</span>" : "<span style=\"color:#666\">");
        } else if (!strcmp(t, "br")) {
            mb_puts(out, "<br/>");
        } else if (!close && (!strcmp(t, "ref") || !strcmp(t, "url"))) {
            const char *e = strstr(s + i, close ? "" : (t[0] == 'r' ? "[/ref]" : "[/url]"));
            size_t endpos = e && (size_t)(e - s) <= n ? (size_t)(e - s) : n;
            put_link(out, s + i, endpos - i);
            i = endpos + (e ? 6 : 0);
            if (i > n) i = n;
        } else if (!close && !strcmp(t, "s")) {
            /* archivo multimedia: se omite */
            const char *e = strstr(s + i, "[/s]");
            i = e && (size_t)(e - s) <= n ? (size_t)(e - s) + 4 : n;
        }
        /* el resto (trn, !trs, *, lang, t, preview…) se ignora conservando el texto */
    }
    while (open_m--) mb_puts(out, "</div>");
    if (!has_m) mb_puts(out, "</div>");
}

static void dsl_render(const char *e, size_t n, const char *word, mbuf *out)
{
    const char *p = e, *end = e + n;
    mbuf hw = {0}, tmp = {0};
    int body_started = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        size_t ln = (size_t)(le - p);
        while (ln && p[ln - 1] == '\r') ln--;
        if (ln) {
            if (p[0] == ' ' || p[0] == '\t') {
                body_started = 1;
                const char *q = p;
                size_t qn = ln;
                while (qn && (*q == ' ' || *q == '\t')) { q++; qn--; }
                dsl_line_html(q, qn, hw.n ? hw.p : word, out);
            } else if (!body_started && !hw.n) {
                dsl_variants(p, ln, &hw, &tmp);
            }
        }
        p = nl ? nl + 1 : end;
    }
    mb_free(&hw);
    mb_free(&tmp);
}

/* ====================================================================== */
/* XDXF                                                                    */
/* ====================================================================== */

static char *xml_text_between(const char *s, const char *open, const char *close)
{
    const char *a = strstr(s, open);
    if (!a) return NULL;
    a = strchr(a, '>');
    if (!a) return NULL;
    a++;
    const char *b = strstr(a, close);
    if (!b) return NULL;
    return md_strndup(a, (size_t)(b - a));
}

static char *xml_attr(const char *s, const char *tag, const char *attr)
{
    const char *t = strstr(s, tag);
    if (!t) return NULL;
    const char *te = strchr(t, '>');
    char pat[64];
    snprintf(pat, sizeof pat, "%s=\"", attr);
    const char *a = strstr(t, pat);
    if (!a || (te && a > te)) return NULL;
    a += strlen(pat);
    const char *b = strchr(a, '"');
    return b ? md_strndup(a, (size_t)(b - a)) : NULL;
}

/* Quita etiquetas y decodifica entidades básicas. skip_opt: quita además <opt>…</opt>. */
static void xml_plain(const char *s, size_t n, int skip_opt, mbuf *out)
{
    int depth_opt = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '<') {
            const char *gt = memchr(s + i, '>', n - i);
            if (!gt) break;
            if (!strncmp(s + i, "<opt", 4)) depth_opt++;
            else if (!strncmp(s + i, "</opt", 5) && depth_opt) depth_opt--;
            i = (size_t)(gt - s);
            continue;
        }
        if (skip_opt && depth_opt) continue;
        if (s[i] == '&') {
            static const struct { const char *e; char c; } ents[] = {
                {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}, {NULL, 0}};
            int done = 0;
            for (int k = 0; ents[k].e; k++) {
                size_t el = strlen(ents[k].e);
                if (i + el <= n && !strncmp(s + i, ents[k].e, el)) {
                    mb_putc(out, ents[k].c);
                    i += el - 1;
                    done = 1;
                    break;
                }
            }
            if (done) continue;
        }
        mb_putc(out, s[i]);
    }
    mb_cstr(out);
}

static void xdxf_add_keys(sidx_builder *b, const char *ar, size_t n, uint64_t off, uint32_t len)
{
    const char *p = ar, *end = ar + n;
    mbuf k1 = {0}, k2 = {0};
    while (p < end) {
        const char *k = strstr(p, "<k");
        if (!k || k >= end) break;
        if (k[2] != '>' && k[2] != ' ') { p = k + 2; continue; }
        const char *ks = strchr(k, '>');
        const char *ke = ks ? strstr(ks, "</k>") : NULL;
        if (!ks || !ke || ke > end) break;
        ks++;
        k1.n = k2.n = 0;
        xml_plain(ks, (size_t)(ke - ks), 0, &k1);
        xml_plain(ks, (size_t)(ke - ks), 1, &k2);
        sidx_add(b, k1.p, k1.n, off, len);
        if (k2.n != k1.n) sidx_add(b, k2.p, k2.n, off, len);
        p = ke + 4;
    }
    mb_free(&k1);
    mb_free(&k2);
}

static int xdxf_build(rfile *rf, sidx_builder *b, char **name, char **lf, char **lt, char *err, int errlen)
{
    (void)err; (void)errlen;
    linereader lr;
    lr_init(&lr, rf);
    mbuf line = {0}, header = {0}, ent = {0};
    uint64_t s, e, ar_start = 0;
    int in_ar = 0, header_done = 0;
    while (lr_next(&lr, &line, &s, &e)) {
        const char *l = line.p;
        size_t pos = 0;
        if (!header_done && !in_ar) {
            const char *a = strstr(l, "<ar");
            size_t hn = a ? (size_t)(a - l) : line.n;
            if (header.n < 65536) { mb_put(&header, l, hn); mb_putc(&header, '\n'); }
            if (!a) continue;
            header_done = 1;
        }
        while (pos <= line.n) {
            if (!in_ar) {
                const char *a = strstr(l + pos, "<ar");
                while (a && a[3] != '>' && a[3] != ' ') a = strstr(a + 3, "<ar");
                if (!a) break;
                in_ar = 1;
                ar_start = s + (uint64_t)(a - l);
                ent.n = 0;
                pos = (size_t)(a - l);
            }
            const char *c = strstr(l + pos, "</ar>");
            if (!c) {
                mb_put(&ent, l + pos, line.n - pos);
                mb_putc(&ent, '\n');
                break;
            }
            size_t cend = (size_t)(c - l) + 5;
            mb_put(&ent, l + pos, cend - pos);
            uint64_t ar_end = s + cend;
            xdxf_add_keys(b, ent.p, ent.n, ar_start, (uint32_t)(ar_end - ar_start));
            in_ar = 0;
            pos = cend;
        }
    }
    mb_cstr(&header);
    char *nm = xml_text_between(header.p, "<full_name", "</full_name>");
    if (!nm) nm = xml_text_between(header.p, "<full_title", "</full_title>");
    if (nm) { mbuf t = {0}; xml_plain(nm, strlen(nm), 0, &t); free(nm); free(*name); *name = mb_steal(&t); }
    char *a = xml_attr(header.p, "<xdxf", "lang_from");
    if (a) { free(*lf); *lf = a; }
    a = xml_attr(header.p, "<xdxf", "lang_to");
    if (a) { free(*lt); *lt = a; }
    mb_free(&line);
    mb_free(&header);
    mb_free(&ent);
    lr_free(&lr);
    return 0;
}

static void xdxf_render(const char *s, size_t n, const char *word, mbuf *out)
{
    (void)word;
    static const struct { const char *from, *to; } map[] = {
        {"<k>", "<b>"}, {"</k>", "</b><br/>"}, {"<ex>", "<i style=\"color:#555\">"}, {"</ex>", "</i>"},
        {"<abr>", "<i style=\"color:green\">"}, {"</abr>", "</i>"}, {"<abbr>", "<i style=\"color:green\">"},
        {"</abbr>", "</i>"}, {"<tr>", "["}, {"</tr>", "]"}, {"<co>", "<span style=\"color:#666\">"},
        {"</co>", "</span>"}, {"<gr>", "<i>"}, {"</gr>", "</i>"}, {"<def>", "<div style=\"margin-left:1em\">"},
        {"</def>", "</div>"}, {"<dtrn>", ""}, {"</dtrn>", ""}, {"<opt>", ""}, {"</opt>", ""},
        {"<ar>", "<div>"}, {"</ar>", "</div>"}, {"<pos>", "<i style=\"color:green\">"}, {"</pos>", "</i>"},
        {NULL, NULL}};
    size_t i = 0;
    while (i < n) {
        if (s[i] == '\n') { mb_puts(out, "<br/>"); i++; continue; }
        if (s[i] != '<') { mb_putc(out, s[i]); i++; continue; }
        int done = 0;
        for (int k = 0; map[k].from; k++) {
            size_t fl = strlen(map[k].from);
            if (i + fl <= n && !strncmp(s + i, map[k].from, fl)) {
                mb_puts(out, map[k].to);
                i += fl;
                done = 1;
                break;
            }
        }
        if (done) continue;
        if (!strncmp(s + i, "<kref", 5)) {
            const char *gt = memchr(s + i, '>', n - i);
            const char *ce = gt ? strstr(gt, "</kref>") : NULL;
            if (gt && ce && (size_t)(ce - s) < n) {
                mbuf t = {0};
                xml_plain(gt + 1, (size_t)(ce - gt - 1), 0, &t);
                put_link(out, t.p, t.n);
                mb_free(&t);
                i = (size_t)(ce - s) + 7;
                continue;
            }
        }
        if (!strncmp(s + i, "<c ", 3) || !strncmp(s + i, "<c>", 3)) {
            const char *gt = memchr(s + i, '>', n - i);
            const char *cc = strstr(s + i, "c=\"");
            if (cc && gt && cc < gt) {
                const char *q = strchr(cc + 3, '"');
                mb_puts(out, "<span style=\"color:");
                mb_put(out, cc + 3, (size_t)(q - cc - 3));
                mb_puts(out, "\">");
            } else {
                mb_puts(out, "<span style=\"color:green\">");
            }
            i = gt ? (size_t)(gt - s) + 1 : n;
            continue;
        }
        if (!strncmp(s + i, "</c>", 4)) { mb_puts(out, "</span>"); i += 4; continue; }
        /* <b>, <i>, <u>, <sub>, <sup>, <br/> y demás se pasan tal cual; las desconocidas se eliminan */
        const char *gt = memchr(s + i, '>', n - i);
        if (!gt) break;
        size_t tl = (size_t)(gt - (s + i)) + 1;
        static const char *pass[] = {"<b>", "</b>", "<i>", "</i>", "<u>", "</u>", "<sub>", "</sub>",
                                     "<sup>", "</sup>", "<br/>", "<br>", NULL};
        for (int k = 0; pass[k]; k++)
            if (strlen(pass[k]) == tl && !strncmp(s + i, pass[k], tl)) { mb_put(out, s + i, tl); break; }
        i += tl;
    }
}

/* ====================================================================== */
/* TSV / tabfile                                                           */
/* ====================================================================== */

static int tab_build(rfile *rf, sidx_builder *b, char **name, char **lf, char **lt, char *err, int errlen)
{
    (void)err; (void)errlen;
    linereader lr;
    lr_init(&lr, rf);
    mbuf line = {0};
    uint64_t s, e;
    while (lr_next(&lr, &line, &s, &e)) {
        const char *l = line.p;
        const char *tab = strchr(l, '\t');
        if (!tab) continue;
        if (l[0] == '#' && l[1] == '#') {
            char *k = md_strndup(l + 2, (size_t)(tab - l - 2));
            char **dst = !strcmp(k, "name") || !strcmp(k, "title") ? name
                       : !strcmp(k, "sourceLang") ? lf : !strcmp(k, "targetLang") ? lt : NULL;
            if (dst) { free(*dst); *dst = md_strdup(tab + 1); }
            free(k);
            continue;
        }
        /* alternativas separadas por "|" */
        const char *p = l;
        while (p < tab) {
            const char *bar = memchr(p, '|', (size_t)(tab - p));
            const char *we = bar ? bar : tab;
            sidx_add(b, p, (size_t)(we - p), s, (uint32_t)(e - s));
            p = we + 1;
        }
    }
    mb_free(&line);
    lr_free(&lr);
    return 0;
}

static void tab_render(const char *s, size_t n, const char *word, mbuf *out)
{
    (void)word;
    const char *tab = memchr(s, '\t', n);
    if (!tab) return;
    const char *p = tab + 1, *end = s + n;
    while (end > p && (end[-1] == '\n' || end[-1] == '\r')) end--;
    for (; p < end; p++) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            if (*p == 'n') mb_puts(out, "<br/>");
            else if (*p == 't') mb_putc(out, '\t');
            else mb_putc(out, *p);
            continue;
        }
        mb_putc(out, *p);
    }
}

/* ====================================================================== */
/* StarDict textual (.xml)                                                 */
/*   <stardict><info><bookname>…</bookname></info>                         */
/*   <article><key>…</key><synonym>…</synonym>                             */
/*            <definition type="h|m|x…"><![CDATA[…]]></definition></article> */
/* ====================================================================== */

/* Texto de un nodo XML: quita CDATA y decodifica entidades. */
static void xml_node_text(const char *s, size_t n, mbuf *out)
{
    out->n = 0;
    const char *p = s, *end = s + n;
    while (p < end) {
        const char *cd = strstr(p, "<![CDATA[");
        if (!cd || cd >= end) { xml_plain(p, (size_t)(end - p), 0, out); break; }
        xml_plain(p, (size_t)(cd - p), 0, out);
        const char *ce = strstr(cd + 9, "]]>");
        if (!ce || ce > end) ce = end;
        mb_put(out, cd + 9, (size_t)(ce - cd - 9));
        p = ce + 3;
    }
    /* recorta espacios */
    size_t a = 0;
    while (a < out->n && (out->p[a] == ' ' || out->p[a] == '\n' || out->p[a] == '\r' || out->p[a] == '\t')) a++;
    if (a) { memmove(out->p, out->p + a, out->n - a); out->n -= a; }
    while (out->n && (out->p[out->n - 1] == ' ' || out->p[out->n - 1] == '\n' || out->p[out->n - 1] == '\r' || out->p[out->n - 1] == '\t')) out->n--;
    mb_cstr(out);
}

static const char *md_memmem(const char *h, size_t hn, const char *n, size_t nn)
{
    if (!nn || hn < nn) return NULL;
    for (const char *p = h, *e = h + hn - nn + 1; (p = memchr(p, n[0], (size_t)(e - p))); p++)
        if (!memcmp(p, n, nn)) return p;
    return NULL;
}

/* Busca `needle` en [p, end) saltando las secciones CDATA, cuyo contenido
 * puede tener etiquetas con el mismo nombre (p. ej. un <article> en HTML).
 * *in_cdata guarda el estado entre llamadas (para leer línea a línea). */
static const char *find_outside_cdata(const char *p, const char *end, const char *needle, int *in_cdata)
{
    size_t nl = strlen(needle);
    while (p < end) {
        if (*in_cdata) {
            const char *ce = md_memmem(p, (size_t)(end - p), "]]>", 3);
            if (!ce) return NULL;
            *in_cdata = 0;
            p = ce + 3;
            continue;
        }
        const char *a = md_memmem(p, (size_t)(end - p), needle, nl);
        const char *cd = md_memmem(p, (size_t)(end - p), "<![CDATA[", 9);
        if (cd && (!a || cd < a)) { *in_cdata = 1; p = cd + 9; continue; }
        return a;
    }
    return NULL;
}

/* Recorre <tag>…</tag> dentro de [s, end) llamando a fn con el contenido. */
static void each_tag(const char *s, const char *end, const char *tag, void (*fn)(void *, const char *, size_t, const char *), void *ud)
{
    char open[32], close[32];
    snprintf(open, sizeof open, "<%s", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    size_t ol = strlen(open);
    const char *p = s;
    while (p < end) {
        int cd = 0;
        const char *a = find_outside_cdata(p, end, open, &cd);
        if (!a) break;
        if (a + ol >= end || (a[ol] != '>' && a[ol] != ' ' && a[ol] != '/')) { p = a + ol; continue; }
        const char *gt = memchr(a, '>', (size_t)(end - a));
        if (!gt) break;
        if (gt[-1] == '/') { p = gt + 1; continue; }
        cd = 0;
        const char *c = find_outside_cdata(gt + 1, end, close, &cd);
        if (!c) break;
        fn(ud, gt + 1, (size_t)(c - gt - 1), a);
        p = c + strlen(close);
    }
}

typedef struct { sidx_builder *b; uint64_t off; uint32_t len; mbuf tmp; } sdt_ctx;

static void sdt_key_cb(void *ud, const char *s, size_t n, const char *tagstart)
{
    (void)tagstart;
    sdt_ctx *c = ud;
    xml_node_text(s, n, &c->tmp);
    if (c->tmp.n) sidx_add(c->b, c->tmp.p, c->tmp.n, c->off, c->len);
}

/* Lector genérico de bloques <tag>…</tag> línea a línea. cb recibe el bloque y sus offsets. */
typedef void (*block_cb)(void *ud, const char *blk, size_t n, uint64_t start, uint64_t end);

static void scan_blocks(rfile *rf, const char *tag, mbuf *header, block_cb cb, void *ud)
{
    linereader lr;
    lr_init(&lr, rf);
    mbuf line = {0}, ent = {0};
    char open[32], close[32];
    snprintf(open, sizeof open, "<%s", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    size_t ol = strlen(open), cl = strlen(close);
    uint64_t s, e, start = 0;
    int in_blk = 0, header_done = 0, in_cdata = 0;
    while (lr_next(&lr, &line, &s, &e)) {
        const char *l = line.p;
        size_t pos = 0;
        while (pos <= line.n) {
            if (!in_blk) {
                const char *a = strstr(l + pos, open);
                while (a && a[ol] != '>' && a[ol] != ' ') a = strstr(a + ol, open);
                if (!header_done && header && header->n < 65536) {
                    size_t hn = a ? (size_t)(a - (l + pos)) : line.n - pos;
                    mb_put(header, l + pos, hn);
                    mb_putc(header, '\n');
                }
                if (!a) break;
                header_done = 1;
                in_blk = 1;
                start = s + (uint64_t)(a - l);
                ent.n = 0;
                pos = (size_t)(a - l);
                in_cdata = 0;
            }
            /* el cierre del bloque no cuenta si está dentro de un CDATA */
            const char *c = find_outside_cdata(l + pos + (size_t)(ent.n ? 0 : ol), l + line.n, close, &in_cdata);
            if (!c) {
                mb_put(&ent, l + pos, line.n - pos);
                mb_putc(&ent, '\n');
                break;
            }
            size_t cend = (size_t)(c - l) + cl;
            mb_put(&ent, l + pos, cend - pos);
            cb(ud, ent.p, ent.n, start, s + cend);
            in_blk = 0;
            pos = cend;
        }
    }
    if (header) mb_cstr(header);
    mb_free(&line);
    mb_free(&ent);
    lr_free(&lr);
}

static void sdt_block(void *ud, const char *blk, size_t n, uint64_t start, uint64_t end)
{
    sdt_ctx *c = ud;
    c->off = start;
    c->len = (uint32_t)(end - start);
    each_tag(blk, blk + n, "key", sdt_key_cb, c);
    each_tag(blk, blk + n, "synonym", sdt_key_cb, c);
}

static int sdt_build(rfile *rf, sidx_builder *b, char **name, char **lf, char **lt, char *err, int errlen)
{
    (void)err; (void)errlen; (void)lf; (void)lt;
    sdt_ctx c = {b, 0, 0, {0}};
    mbuf header = {0};
    scan_blocks(rf, "article", &header, sdt_block, &c);
    char *nm = xml_text_between(header.p ? header.p : "", "<bookname", "</bookname>");
    if (nm) { xml_node_text(nm, strlen(nm), &c.tmp); free(*name); *name = md_strndup(c.tmp.p, c.tmp.n); free(nm); }
    mb_free(&header);
    mb_free(&c.tmp);
    return 0;
}

typedef struct { mbuf *out; mbuf tmp; } sdt_render_ctx;

static void sdt_def_cb(void *ud, const char *s, size_t n, const char *tagstart)
{
    sdt_render_ctx *c = ud;
    char type = 'm';
    const char *ty = strstr(tagstart, "type=\"");
    const char *gt = strchr(tagstart, '>');
    if (ty && gt && ty < gt) type = ty[6];
    xml_node_text(s, n, &c->tmp);
    mb_puts(c->out, "<div>");
    if (type == 'h' || type == 'g') {
        mb_put(c->out, c->tmp.p, c->tmp.n);
    } else if (type == 'x') {
        xdxf_render(c->tmp.p, c->tmp.n, "", c->out);
    } else {
        for (size_t i = 0; i < c->tmp.n; i++) {
            if (c->tmp.p[i] == '\n') mb_puts(c->out, "<br/>");
            else mb_html_escape(c->out, c->tmp.p + i, 1);
        }
    }
    mb_puts(c->out, "</div>");
}

static void sdt_render(const char *s, size_t n, const char *word, mbuf *out)
{
    (void)word;
    sdt_render_ctx c = {out, {0}};
    each_tag(s, s + n, "definition", sdt_def_cb, &c);
    mb_free(&c.tmp);
}

/* ====================================================================== */
/* CSV (como el de pyglossary): palabra, definición[, alternativas]        */
/* Filas "#clave","valor" al principio = metadatos. Separador , o ;        */
/* ====================================================================== */

/* Parsea un campo CSV desde *pp; devuelve 1 si hay campo. */
static int csv_field(const char **pp, const char *end, char delim, mbuf *out)
{
    const char *p = *pp;
    out->n = 0;
    if (p > end) return 0;
    if (p < end && *p == '"') {
        p++;
        while (p < end) {
            if (*p == '"') {
                if (p + 1 < end && p[1] == '"') { mb_putc(out, '"'); p += 2; continue; }
                p++;
                break;
            }
            mb_putc(out, *p++);
        }
        while (p < end && *p != delim && *p != '\n') p++;
    } else {
        while (p < end && *p != delim && *p != '\n' && *p != '\r') mb_putc(out, *p++);
    }
    mb_cstr(out);
    if (p < end && *p == delim) p++;
    else p = end + 1;
    *pp = p;
    return 1;
}

static char csv_delim_of(const char *s, size_t n)
{
    /* el separador es el carácter que sigue al primer campo */
    const char *p = s, *end = s + n;
    if (p < end && *p == '"') {
        p++;
        while (p < end) {
            if (*p == '"') { if (p + 1 < end && p[1] == '"') { p += 2; continue; } p++; break; }
            p++;
        }
        return (p < end && (*p == ';' || *p == '\t')) ? *p : ',';
    }
    while (p < end && *p != ',' && *p != ';' && *p != '\t' && *p != '\n') p++;
    return (p < end && (*p == ';' || *p == '\t')) ? *p : ',';
}

static int quotes_open(const char *s, size_t n)
{
    int open = 0;
    for (size_t i = 0; i < n; i++) if (s[i] == '"') open = !open;
    return open;
}

static int csv_build(rfile *rf, sidx_builder *b, char **name, char **lf, char **lt, char *err, int errlen)
{
    (void)err; (void)errlen;
    linereader lr;
    lr_init(&lr, rf);
    mbuf line = {0}, rec = {0}, f = {0};
    uint64_t s, e, start = 0;
    int pending = 0;
    char delim = 0;
    while (lr_next(&lr, &line, &s, &e)) {
        if (!pending) { rec.n = 0; start = s; }
        else mb_putc(&rec, '\n');
        mb_put(&rec, line.p, line.n);
        pending = quotes_open(rec.p, rec.n);
        if (pending) continue;
        if (!rec.n) continue;
        if (!delim) delim = csv_delim_of(rec.p, rec.n);
        const char *p = rec.p, *end = rec.p + rec.n;
        if (!csv_field(&p, end, delim, &f) || !f.n) continue;
        if (f.p[0] == '#') {
            char *key = md_strdup(f.p + 1);
            if (csv_field(&p, end, delim, &f)) {
                char **dst = !strcmp(key, "name") || !strcmp(key, "title") ? name
                           : !strcmp(key, "sourceLang") ? lf : !strcmp(key, "targetLang") ? lt : NULL;
                if (dst) { free(*dst); *dst = md_strndup(f.p, f.n); }
            }
            free(key);
            continue;
        }
        uint32_t len = (uint32_t)(e - start);
        sidx_add(b, f.p, f.n, start, len);
        if (csv_field(&p, end, delim, &f) && csv_field(&p, end, delim, &f) && f.n) {
            /* alternativas separadas por comas */
            const char *a = f.p, *fe = f.p + f.n;
            while (a < fe) {
                const char *c = memchr(a, ',', (size_t)(fe - a));
                const char *ae = c ? c : fe;
                sidx_add(b, a, (size_t)(ae - a), start, len);
                a = ae + 1;
            }
        }
    }
    mb_free(&line);
    mb_free(&rec);
    mb_free(&f);
    lr_free(&lr);
    return 0;
}

static void csv_render(const char *s, size_t n, const char *word, mbuf *out)
{
    (void)word;
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    char delim = csv_delim_of(s, n);
    const char *p = s, *end = s + n;
    mbuf f = {0};
    if (csv_field(&p, end, delim, &f) && csv_field(&p, end, delim, &f)) mb_put(out, f.p, f.n);
    mb_free(&f);
}

/* ====================================================================== */
/* comunes                                                                 */
/* ====================================================================== */

static int detect_enc(rfile *rf)
{
    unsigned char p[4] = {0};
    size_t n = rf_pread(rf, 0, p, 4);
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) return ENC_UTF16LE;
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) return ENC_UTF16BE;
    if (n >= 4 && p[1] == 0 && p[3] == 0 && p[0]) return ENC_UTF16LE;
    if (n >= 4 && p[0] == 0 && p[2] == 0 && p[1]) return ENC_UTF16BE;
    return ENC_UTF8;
}

static const build_fn builders[] = {dsl_build, xdxf_build, tab_build, sdt_build, csv_build};
static const render_fn renderers[] = {dsl_render, xdxf_render, tab_render, sdt_render, csv_render};

static int text_needs_index(const char *path, const char *cache_dir)
{
    char *p = sidx_path_for(path, cache_dir, 0);
    int fresh = sidx_is_fresh(p, path);
    free(p);
    if (fresh) return 0;
    p = sidx_path_for(path, cache_dir, 1);
    fresh = sidx_is_fresh(p, path);
    free(p);
    return !fresh;
}

static int text_open(md_dict *d, const char *cache_dir, int kind, char *err, int errlen)
{
    textdict *t = calloc(1, sizeof *t);
    t->kind = kind;
    t->rf = rf_open(d->path, err, errlen);
    if (!t->rf) { free(t); return -1; }
    t->enc = detect_enc(t->rf);

    char *ipath = sidx_path_for(d->path, cache_dir, 0);
    if (!sidx_is_fresh(ipath, d->path)) {
        free(ipath);
        ipath = sidx_path_for(d->path, cache_dir, 1);
        if (!sidx_is_fresh(ipath, d->path)) {
            sidx_builder *b = sidx_new();
            char *name = NULL, *lf = NULL, *lt = NULL;
            int rc = builders[kind](t->rf, b, &name, &lf, &lt, err, errlen);
            if (!rc) rc = sidx_write(b, ipath, d->path, name, lf, lt, err, errlen);
            sidx_free(b);
            free(name); free(lf); free(lt);
            if (rc) { free(ipath); rf_close(t->rf); free(t); return -1; }
        }
    }
    char *name = NULL, *lf = NULL, *lt = NULL;
    t->idx = sidx_open(ipath, &name, &lf, &lt, &d->word_count, err, errlen);
    free(ipath);
    if (!t->idx) { rf_close(t->rf); free(t); return -1; }
    if (name && *name) d->name = name; else free(name);
    d->lang_from = lf;
    d->lang_to = lt;
    d->is_html = 1;
    d->priv = t;
    return 0;
}

static void text_close(md_dict *d)
{
    textdict *t = d->priv;
    if (!t) return;
    sidx_close(t->idx);
    rf_close(t->rf);
    free(t);
}

static int text_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    textdict *t = d->priv;
    sidx_hit *hits = calloc((size_t)max, sizeof *hits);
    int n = sidx_find(t->idx, word, fuzzy, hits, max);
    mbuf raw = {0}, txt = {0}, html = {0};
    for (int i = 0; i < n; i++) {
        raw.n = txt.n = html.n = 0;
        if (rf_read(t->rf, hits[i].off, hits[i].len, &raw) == 0) {
            decode_raw(raw.p, raw.n, t->enc, &txt);
            mb_cstr(&txt);
            renderers[t->kind](txt.p, txt.n, hits[i].word, &html);
            mb_cstr(&html);
            if (!md_results_has(r, hits[i].word, html.p))
                md_results_add(r, hits[i].word, strlen(hits[i].word), html.p, html.n);
        }
        free(hits[i].word);
    }
    free(hits);
    mb_free(&raw);
    mb_free(&txt);
    mb_free(&html);
    return r->n;
}

/* ---------------------------------------------------------------------- */

static int dsl_probe(const char *p)
{
    if (strstr(p, "_abrv.dsl")) return 0; /* archivos de abreviaturas */
    return ends_with_ci(p, ".dsl") || ends_with_ci(p, ".dsl.dz");
}
static int xdxf_probe(const char *p) { return ends_with_ci(p, ".xdxf") || ends_with_ci(p, ".xdxf.dz"); }
static int tab_probe(const char *p) { return ends_with_ci(p, ".tab") || ends_with_ci(p, ".tsv") || ends_with_ci(p, ".tab.dz"); }

/* .xml solo si empieza como un StarDict textual */
static int sdt_probe(const char *p)
{
    if (!ends_with_ci(p, ".xml")) return 0;
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, "<stardict") != NULL;
}
static int csv_probe(const char *p) { return ends_with_ci(p, ".csv"); }

static int dsl_open(md_dict *d, const char *c, char *e, int l) { return text_open(d, c, 0, e, l); }
static int sdt_open(md_dict *d, const char *c, char *e, int l) { return text_open(d, c, 3, e, l); }
static int csv_open(md_dict *d, const char *c, char *e, int l) { return text_open(d, c, 4, e, l); }
static int xdxf_open(md_dict *d, const char *c, char *e, int l) { return text_open(d, c, 1, e, l); }
static int tab_open(md_dict *d, const char *c, char *e, int l) { return text_open(d, c, 2, e, l); }

const md_format_t md_fmt_dsl = {"dsl", dsl_probe, text_needs_index, dsl_open, text_close, text_lookup};
const md_format_t md_fmt_xdxf = {"xdxf", xdxf_probe, text_needs_index, xdxf_open, text_close, text_lookup};
const md_format_t md_fmt_tab = {"tab", tab_probe, text_needs_index, tab_open, text_close, text_lookup};
const md_format_t md_fmt_sdt = {"stardict-xml", sdt_probe, text_needs_index, sdt_open, text_close, text_lookup};
const md_format_t md_fmt_csv = {"csv", csv_probe, text_needs_index, csv_open, text_close, text_lookup};
