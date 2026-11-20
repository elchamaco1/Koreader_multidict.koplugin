/* Babylon (.bgl).
 *
 * Un .bgl es una cabecera de 6 bytes seguida de un único flujo gzip con una
 * serie de bloques (tipo 0/3 = información, 1/7/10/11/13 = entradas,
 * 2 = recursos). No tiene índice ni permite acceso aleatorio, así que la
 * primera vez se descomprime, se crea el índice auxiliar (.mdidx) y se guarda
 * una copia de los bloques en trozos LZ4 de 64 KB (.mddat) para poder leer
 * cada artículo directamente después.
 *
 * Conversión de claves y definiciones según pyglossary/plugins/babylon_bgl.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "md_internal.h"
#include "lz4/lz4.h"
#include "bgl_tables.h"

#define DAT_MAGIC "MDBGLDT1"
#define DAT_CHUNK 65536

typedef struct {
    char src_enc[16], tgt_enc[16];
    sidx *idx;
    FILE *dat;
    uint32_t nchunks;
    uint64_t raw_size;
    uint64_t *choff;
    long cchunk;
    uint8_t *cbuf;
    uint8_t chunk[DAT_CHUNK];
    size_t chunk_len;
} bgl;

/* ---------------------------------------------------------------------- */
/* codificaciones                                                          */
/* ---------------------------------------------------------------------- */

/* Decodifica a UTF-8. replace=1: bytes inválidos -> U+FFFD; 0: se omiten. */
static void bgl_decode(const char *enc, const uint8_t *s, size_t n, int replace, mbuf *out)
{
    if (!strcmp(enc, "utf-8")) {
        const char *p = (const char *)s, *e = p + n;
        while (p < e) {
            const char *q = p;
            uint32_t cp = utf8_next(&p, e);
            if (cp == 0xFFFD && !(e - q >= 3 && (uint8_t)q[0] == 0xEF && (uint8_t)q[1] == 0xBF && (uint8_t)q[2] == 0xBD)) {
                if (replace) utf8_put(out, 0xFFFD);
            } else mb_put(out, q, (size_t)(p - q));
        }
        return;
    }
    for (int t = 0; t < (int)(sizeof bgl_sb_names / sizeof *bgl_sb_names); t++) {
        if (strcmp(enc, bgl_sb_names[t])) continue;
        for (size_t i = 0; i < n; i++) {
            if (s[i] < 0x80) { mb_putc(out, (char)s[i]); continue; }
            uint16_t cp = bgl_sb_tab[t][s[i] - 0x80];
            if (cp != 0xFFFD || replace) utf8_put(out, cp);
        }
        return;
    }
    /* páginas multibyte (CJK): iconv con nombres alternativos */
    static const char *const alt[][3] = {
        {"cp932", "CP932", "SHIFT_JIS"}, {"cp936", "CP936", "GBK"}, {"gbk", "GBK", "CP936"},
        {"cp949", "CP949", "EUC-KR"}, {"cp950", "CP950", "BIG5"},
    };
    for (int i = 0; i < 5; i++) {
        if (strcmp(enc, alt[i][0])) continue;
        size_t keep = out->n;
        for (int k = 1; k < 3; k++) {
            out->n = keep;
            if (!iconv_to_utf8(alt[i][k], (const char *)s, n, out)) return;
        }
        out->n = keep;
    }
    /* sin conversor: solo ASCII */
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x80) mb_putc(out, (char)s[i]);
        else if (replace) utf8_put(out, 0xFFFD);
    }
}

/* ---------------------------------------------------------------------- */
/* utilidades de texto (bgl_text.py)                                       */
/* ---------------------------------------------------------------------- */

static int is_word_cp(uint32_t cp)
{
    if (cp < 0x80) return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_';
    return md_lower(cp) != cp || cp >= 0xC0;  /* aproximación de \w para no ASCII */
}

static int is_py_space(uint32_t cp)
{
    return cp == ' ' || (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x1F) || cp == 0x85 || cp == 0xA0 ||
           cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000;
}

static void strip_py(mbuf *b, int left, int right)
{
    const char *p = b->p, *e = b->p + b->n;
    if (left)
        while (p < e) {
            const char *q = p;
            if (!is_py_space(utf8_next(&q, e))) break;
            p = q;
        }
    const char *end = e;
    if (right) {
        const char *q = p, *last = p;
        while (q < e) {
            uint32_t cp = utf8_next(&q, e);
            if (!is_py_space(cp)) last = q;
        }
        end = last;
    }
    size_t n = (size_t)(end - p);
    memmove(b->p, p, n);
    b->n = n;
    mb_cstr(b);
}

static void remove_control(mbuf *b)
{
    size_t j = 0;
    for (size_t i = 0; i < b->n; i++) {
        uint8_t c = (uint8_t)b->p[i];
        if (c <= 0x08 || c == 0x0C || (c >= 0x0E && c <= 0x1F)) continue;
        b->p[j++] = b->p[i];
    }
    b->n = j;
    mb_cstr(b);
}

/* [\r\n]+ -> rep */
static void collapse_newlines(mbuf *b, char rep)
{
    size_t j = 0;
    for (size_t i = 0; i < b->n; i++) {
        if (b->p[i] == '\r' || b->p[i] == '\n') {
            while (i + 1 < b->n && (b->p[i + 1] == '\r' || b->p[i + 1] == '\n')) i++;
            b->p[j++] = rep;
        } else b->p[j++] = b->p[i];
    }
    b->n = j;
    mb_cstr(b);
}

static void remove_chars(mbuf *b, char a, char c)
{
    size_t j = 0;
    for (size_t i = 0; i < b->n; i++)
        if (b->p[i] != a && b->p[i] != c) b->p[j++] = b->p[i];
    b->n = j;
    mb_cstr(b);
}

static int ent_lookup(const char *name, size_t n, uint32_t *cp)
{
    char low[16];
    if (n >= sizeof low) return 0;
    for (size_t i = 0; i < n; i++) low[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
    low[n] = 0;
    int lo = 0, hi = (int)(sizeof bgl_ents / sizeof *bgl_ents) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(bgl_ents[mid].name, low);
        if (!c) { *cp = bgl_ents[mid].cp; return 1; }
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* (?:&#x|&#|&)(\w+);?  (in_key: ';' obligatorio y sin escapar el resultado) */
static void replace_entities(mbuf *b, int in_key)
{
    mbuf o = {0};
    const char *p = b->p, *e = b->p + b->n;
    while (p < e) {
        if (*p != '&') { mb_putc(&o, *p++); continue; }
        int kind = 0; /* 0 nombre, 1 decimal, 2 hex */
        const char *nm = p + 1;
        if (nm < e && *nm == '#') {
            if (nm + 1 < e && (nm[1] == 'x' || nm[1] == 'X')) {
                /* "&#x" solo si le sigue \w; si no, se prueba "&#" */
                const char *q = nm + 2;
                uint32_t c = q < e ? utf8_next(&q, e) : 0;
                if (c && is_word_cp(c)) { kind = 2; nm += 2; } else { kind = 1; nm += 1; }
            } else { kind = 1; nm += 1; }
        }
        const char *q = nm, *ne = nm;
        while (q < e) {
            const char *r = q;
            uint32_t c = utf8_next(&r, e);
            if (!is_word_cp(c)) break;
            q = r;
            ne = q;
        }
        if (ne == nm) {
            /* "&#" sin \w: se reintenta con "&" + \w (no hay \w tras "&#"), así que es texto */
            mb_putc(&o, *p++);
            continue;
        }
        int semi = ne < e && *ne == ';';
        if (in_key && !semi) { mb_putc(&o, *p++); continue; }
        const char *end = ne + semi;
        uint32_t cp = 0;
        int ok = 1;
        if (kind) {
            char tmp[24];
            size_t l = (size_t)(ne - nm);
            if (l >= sizeof tmp) cp = 0xFFFD;
            else {
                memcpy(tmp, nm, l);
                tmp[l] = 0;
                char *ep;
                unsigned long long v = strtoull(tmp, &ep, kind == 2 ? 16 : 10);
                if (*ep || v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) cp = 0xFFFD;
                else cp = (uint32_t)v;
            }
        } else ok = ent_lookup(nm, (size_t)(ne - nm), &cp);
        if (!ok) { mb_put(&o, p, (size_t)(end - p)); p = end; continue; }
        if (!in_key && (cp == '<' || cp == '>' || cp == '&'))
            mb_puts(&o, cp == '<' ? "&lt;" : cp == '>' ? "&gt;" : "&amp;");
        else utf8_put(&o, cp);
        p = end;
    }
    mb_cstr(&o);
    mb_free(b);
    *b = o;
}

/* (?:<[/a-zA-Z].*?(?:>|$))+  ->  " " */
static void strip_tags(mbuf *b)
{
    mbuf o = {0};
    size_t i = 0, n = b->n;
    const char *s = b->p;
    while (i < n) {
        size_t j = i, run = 0;
        for (;;) {
            if (j + 1 < n + 1 && j < n && s[j] == '<' && j + 1 < n &&
                (s[j + 1] == '/' || (s[j + 1] >= 'a' && s[j + 1] <= 'z') || (s[j + 1] >= 'A' && s[j + 1] <= 'Z'))) {
                size_t k = j + 1;
                while (k < n && s[k] != '>' && s[k] != '\n') k++;
                if (k < n && s[k] == '>') { j = k + 1; run = 1; continue; }
                if (k == n || (s[k] == '\n' && k == n - 1)) { j = k; run = 1; continue; }
            }
            break;
        }
        if (run) { mb_putc(&o, ' '); i = j; }
        else mb_putc(&o, s[i++]);
    }
    mb_cstr(&o);
    mb_free(b);
    *b = o;
}

/* stripDollarIndexes sobre bytes */
static void strip_dollar(const uint8_t *w, size_t n, mbuf *out)
{
    size_t i = 0;
    for (;;) {
        const uint8_t *d0p = i < n ? memchr(w + i, '$', n - i) : NULL;
        if (!d0p) { mb_put(out, w + i, n - i); break; }
        size_t d0 = (size_t)(d0p - w);
        const uint8_t *d1p = d0 + 1 < n ? memchr(w + d0 + 1, '$', n - d0 - 1) : NULL;
        if (!d1p) { mb_put(out, w + i, n - i); break; }
        size_t d1 = (size_t)(d1p - w);
        if (d1 == d0 + 1) {
            mb_put(out, w + i, d0 - i);
            i = d1 + 1;
            while (i < n && w[i] == '$') i++;
            if (i >= n) break;
            continue;
        }
        int digits = 1;
        for (size_t k = d0 + 1; k < d1; k++) if (w[k] < '0' || w[k] > '9') { digits = 0; break; }
        if (!digits) { mb_put(out, w + i, d1 - i); i = d1; continue; }
        mb_put(out, w + i, d0 - i);
        i = d1 + 1;
    }
}

/* ---------------------------------------------------------------------- */
/* claves y definiciones                                                   */
/* ---------------------------------------------------------------------- */

static void process_key(bgl *g, const uint8_t *w, size_t n, int alt, mbuf *out)
{
    mbuf raw = {0};
    strip_dollar(w, n, &raw);
    out->n = 0;
    bgl_decode(g->src_enc, (uint8_t *)raw.p, raw.n, 0, out);
    mb_cstr(out);
    mb_free(&raw);
    if (alt) {
        /* (^|\s)/(\w) -> \1\2 */
        size_t j = 0;
        for (size_t i = 0; i < out->n; i++) {
            if (out->p[i] == '/' && (i == 0 || is_py_space((uint8_t)out->p[i - 1])) && i + 1 < out->n) {
                const char *q = out->p + i + 1;
                if (is_word_cp(utf8_next(&q, out->p + out->n))) continue;
            }
            out->p[j++] = out->p[i];
        }
        out->n = j;
        mb_cstr(out);
        strip_tags(out);
        replace_entities(out, 1);
    } else {
        replace_entities(out, 1);
        strip_tags(out);
    }
    remove_control(out);
    collapse_newlines(out, ' ');
    strip_py(out, 1, !alt);
}

/* replaceAsciiCharRefs: &#NNN; con 128 <= NNN <= 255 -> byte */
static void ascii_refs(const uint8_t *s, size_t n, mbuf *out)
{
    for (size_t i = 0; i < n;) {
        if (s[i] == '&' && i + 1 < n && s[i + 1] == '#') {
            size_t k = i + 2;
            while (k < n && ((s[k] >= '0' && s[k] <= '9') || (s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z') || s[k] == '_')) k++;
            if (k > i + 2 && k < n && s[k] == ';') {
                int digits = 1;
                long v = 0;
                for (size_t m = i + 2; m < k; m++) {
                    if (s[m] < '0' || s[m] > '9') { digits = 0; break; }
                    if (v < 100000) v = v * 10 + (s[m] - '0');
                }
                if (digits && v >= 128 && v <= 255) { mb_putc(out, (char)v); i = k + 1; continue; }
                mb_put(out, s + i, k + 1 - i);
                i = k + 1;
                continue;
            }
        }
        mb_putc(out, (char)s[i++]);
    }
}

static void decode_block(bgl *g, const char *enc, const uint8_t *s, size_t n, mbuf *out)
{
    if (!strcmp(enc, "babylon-reference")) {
        size_t st = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i < n && s[i] != ';') continue;
            if (i - st == 4) {
                uint32_t v = 0;
                int ok = 1;
                for (size_t k = st; k < i; k++) {
                    int c = s[k];
                    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                    if (d < 0) { ok = 0; break; }
                    v = v * 16 + (uint32_t)d;
                }
                if (ok) utf8_put(out, (v >= 0xD800 && v <= 0xDFFF) ? 0xFFFD : v);
            }
            st = i + 1;
        }
        return;
    }
    if (!strcmp(enc, "cp1252")) {
        mbuf t = {0};
        ascii_refs(s, n, &t);
        bgl_decode(enc, (uint8_t *)t.p, t.n, 1, out);
        mb_free(&t);
    } else bgl_decode(enc, s, n, 1, out);
}

static int ieq(const uint8_t *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int c = a[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != b[i]) return 0;
    }
    return 1;
}

/* decodeCharsetTags: <charset c=X> ... </charset> */
static void charset_tags(bgl *g, const uint8_t *s, size_t n, const char *def, mbuf *out)
{
    const char *stack[32];
    int sp = 0;
    size_t st = 0, i = 0;
    out->n = 0;
    while (i < n) {
        size_t tag_end = 0;
        const char *push = NULL;
        int pop = 0;
        if (s[i] == '<') {
            if (i + 10 <= n && ieq(s + i, "</charset>", 10)) { tag_end = i + 10; pop = 1; }
            else if (i + 8 <= n && ieq(s + i, "<charset", 8)) {
                size_t k = i + 8, ws = k;
                while (k < n && (s[k] == ' ' || (s[k] >= 9 && s[k] <= 13))) k++;
                if (k > ws && k + 2 <= n && (s[k] == 'c' || s[k] == 'C') && s[k + 1] == '=') {
                    k += 2;
                    if (k < n && (s[k] == '\'' || s[k] == '"')) k++;
                    if (k < n && ((s[k] >= '0' && s[k] <= '9') || (s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z') || s[k] == '_')) {
                        int t = s[k] | 0x20;
                        k++;
                        if (k < n && s[k] == '"') k++;
                        if (k < n && s[k] == '>') {
                            tag_end = k + 1;
                            push = t == 't' ? "babylon-reference" : t == 'u' ? "utf-8" : (t == 'k' || t == 'e') ? g->src_enc
                                 : t == 'g' ? "gbk" : def;
                        }
                    }
                }
            }
        }
        if (!tag_end) { i++; continue; }
        decode_block(g, sp ? stack[sp - 1] : def, s + st, i - st, out);
        if (pop) { if (sp) sp--; }
        else if (sp < 32) stack[sp++] = push;
        i = st = tag_end;
    }
    decode_block(g, sp ? stack[sp - 1] : def, s + st, n - st, out);
    mb_cstr(out);
}

static void process_defi(bgl *g, const uint8_t *d, size_t n, mbuf *out)
{
    /* inicio de los campos finales: \x14 no seguido de espacio (sin contar el último byte) */
    long f0 = -1;
    for (size_t i = 0; i + 1 < n; i++)
        if (d[i] == 0x14) { f0 = (long)i; if (d[i + 1] != 0x20) break; f0 = -1; }
    size_t body = f0 < 0 ? n : (size_t)f0;
    const char *pos = NULL;
    const uint8_t *title = NULL, *ttrans = NULL, *tr50 = NULL, *tr60 = NULL;
    size_t title_n = 0, ttrans_n = 0, tr50_n = 0, tr60_n = 0;
    int code50 = 0, code60 = 0;
    if (f0 >= 0) {
        size_t i = (size_t)f0 + 1;
        while (i < n) {
            int c = d[i];
            if (c >= 0x40 && c <= 0x4F) {
                size_t l = (size_t)(c - 0x3F);
                if (i + 2 + l > n) break;
                i += 2 + l;
            } else if (c == 0x02) {
                if (i + 1 >= n) break;
                int pc = d[i + 1];
                if (pc < 0x30 || pc > 0x47) break;
                pos = bgl_pos[pc - 0x30];
                i += 2;
            } else if (c == 0x06) {
                if (i + 1 >= n) break;
                i += 2;
            } else if (c == 0x07) {
                if (i + 3 > n) break;
                i += 3;
            } else if (c == 0x13 || c == 0x18 || c == 0x1A) {
                if (i + 1 >= n) break;
                size_t l = d[i + 1];
                i += 2;
                if (!l) continue;
                if (i + l > n) break;
                if (c == 0x18) { title = d + i; title_n = l; }
                i += l;
            } else if (c == 0x28) {
                if (i + 2 >= n) break;
                size_t l = ((size_t)d[i + 1] << 8) | d[i + 2];
                i += 3;
                if (!l) continue;
                if (i + l > n) break;
                ttrans = d + i; ttrans_n = l;
                i += l;
            } else if (c == 0x50) {
                if (i + 2 >= n) break;
                code50 = d[i + 1];
                size_t l = d[i + 2];
                i += 3;
                if (!l) continue;
                if (i + l > n) break;
                tr50 = d + i; tr50_n = l;
                i += l;
            } else if (c == 0x60) {
                if (i + 4 > n) break;
                code60 = d[i + 1];
                size_t l = ((size_t)d[i + 2] << 8) | d[i + 3];
                i += 4;
                if (!l) continue;
                if (i + l > n) break;
                tr60 = d + i; tr60_n = l;
                i += l;
            } else break;
        }
    }
    mbuf defi = {0}, t = {0};
    charset_tags(g, d, body, g->tgt_enc, &defi);
    remove_chars(&defi, 0x1E, 0x1F);
    replace_entities(&defi, 0);
    remove_control(&defi);
    collapse_newlines(&defi, '\n');
    strip_py(&defi, 1, 1);

    out->n = 0;
    if ((pos && *pos) || title) {
        if (pos && *pos) {
            mb_puts(out, "<font color=\"#007000\">");
            mb_html_escape(out, pos, strlen(pos));
            mb_puts(out, "</font>");
        }
        if (title) {
            charset_tags(g, title, title_n, g->src_enc, &t);
            replace_entities(&t, 0);
            remove_control(&t);
            if (t.n) {
                if (out->n) mb_putc(out, ' ');
                mb_put(out, t.p, t.n);
            }
        }
        mb_puts(out, "<br>\n");
    }
    if (ttrans) {
        charset_tags(g, ttrans, ttrans_n, g->src_enc, &t);
        replace_entities(&t, 0);
        remove_control(&t);
        if (t.n) { mb_put(out, t.p, t.n); mb_puts(out, "<br>\n"); }
    }
    const uint8_t *trs[2] = {code50 == 0x1B ? tr50 : NULL, code60 == 0x1B ? tr60 : NULL};
    size_t trn[2] = {tr50_n, tr60_n};
    for (int k = 0; k < 2; k++) {
        if (!trs[k]) continue;
        charset_tags(g, trs[k], trn[k], g->src_enc, &t);
        replace_entities(&t, 0);
        remove_control(&t);
        if (t.n) { mb_putc(out, '['); mb_put(out, t.p, t.n); mb_puts(out, "]<br>\n"); }
    }
    mb_put(out, defi.p, defi.n);
    if (out->n >= 4 && !memcmp(out->p + out->n - 4, "<br>", 4)) out->n -= 4;
    if (out->n >= 4 && !memcmp(out->p + out->n - 4, "<BR>", 4)) out->n -= 4;
    mb_cstr(out);
    mb_free(&defi);
    mb_free(&t);
}

static uint32_t be(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

/* Analiza un bloque de entrada: palabra, definición y alternativas.
 * Devuelve 0 si es válido (como pyglossary: los bloques con errores se omiten). */
typedef struct {
    const uint8_t *word; size_t word_n;
    const uint8_t *defi; size_t defi_n;
    int nalts;
    const uint8_t *alts[256]; size_t alts_n[256];
} bgl_entry;

static int parse_entry(int type, const uint8_t *d, size_t n, bgl_entry *e)
{
    e->nalts = 0;
    size_t pos = 0;
    if (type == 11) {
        if (pos + 5 > n) return -1;
        size_t wl = ((size_t)d[1] << 24) | ((size_t)d[2] << 16) | ((size_t)d[3] << 8) | d[4]; /* 5 bytes, el primero siempre 0 */
        if (d[0]) return -1;
        pos = 5;
        if (pos + wl > n) return -1;
        e->word = d + pos; e->word_n = wl;
        pos += wl;
        if (pos + 4 > n) return -1;
        uint32_t na = be(d + pos, 4);
        pos += 4;
        for (uint32_t i = 0; i < na; i++) {
            if (pos + 4 > n) return -1;
            uint32_t al = be(d + pos, 4);
            pos += 4;
            if (!al) break;
            if (pos + al > n) return -1;
            if (e->nalts < 256) { e->alts[e->nalts] = d + pos; e->alts_n[e->nalts++] = al; }
            pos += al;
        }
        if (pos + 4 > n) return -1;
        uint32_t dl = be(d + pos, 4);
        pos += 4;
        if (pos + dl > n) return -1;
        e->defi = d + pos; e->defi_n = dl;
        return 0;
    }
    if (pos + 1 > n) return -1;
    size_t wl = d[pos++];
    if (pos + wl > n) return -1;
    e->word = d + pos; e->word_n = wl;
    pos += wl;
    if (pos + 2 > n) return -1;
    size_t dl = be(d + pos, 2);
    pos += 2;
    if (pos + dl > n) return -1;
    e->defi = d + pos; e->defi_n = dl;
    pos += dl;
    while (pos < n) {
        size_t al = d[pos++];
        if (pos + al > n) return -1;
        if (e->nalts < 256) { e->alts[e->nalts] = d + pos; e->alts_n[e->nalts++] = al; }
        pos += al;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* apertura e índice                                                       */
/* ---------------------------------------------------------------------- */

static int bgl_probe(const char *p)
{
    if (!ends_with_ci(p, ".bgl")) return 0;
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    uint8_t h[4];
    int ok = fread(h, 1, 4, f) == 4 && h[0] == 0x12 && h[1] == 0x34 && h[2] == 0 && (h[3] == 1 || h[3] == 2);
    fclose(f);
    return ok;
}

static char *dat_path(const char *idx_path)
{
    mbuf b = {0};
    size_t n = strlen(idx_path);
    if (n > 6 && !strcmp(idx_path + n - 6, ".mdidx")) n -= 6;
    mb_put(&b, idx_path, n);
    mb_puts(&b, ".mddat");
    return mb_steal(&b);
}

static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

/* cabecera: magic(8) tamaño_origen(8) mtime(8) raw(8) ntrozos(4) enc_origen(16) enc_destino(16) */
#define DAT_HDR 68

static int dat_fresh(const char *dp, const char *src)
{
    struct stat st;
    if (stat(src, &st)) return 0;
    FILE *f = fopen(dp, "rb");
    if (!f) return 0;
    uint8_t h[DAT_HDR];
    int ok = fread(h, 1, DAT_HDR, f) == DAT_HDR && !memcmp(h, DAT_MAGIC, 8) &&
             get64(h + 8) == (uint64_t)st.st_size && get64(h + 16) == (uint64_t)st.st_mtime;
    fclose(f);
    return ok;
}

static uint8_t *inflate_bgl(const char *path, size_t *raw_n, char *err, int errlen)
{
    mbuf f = {0};
    FILE *fp = fopen(path, "rb");
    if (!fp) { md_seterr(err, errlen, "no se puede abrir %s", path); return NULL; }
    fseeko(fp, 0, SEEK_END);
    off_t sz = ftello(fp);
    fseeko(fp, 0, SEEK_SET);
    if (sz < 16 || mb_reserve(&f, (size_t)sz) || fread(f.p, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp); mb_free(&f);
        md_seterr(err, errlen, "no se puede leer %s", path);
        return NULL;
    }
    fclose(fp);
    f.n = (size_t)sz;
    const uint8_t *d = (const uint8_t *)f.p;
    size_t go = be(d + 4, 2);
    uint8_t *raw = NULL;
    if (go >= 6 && go + 18 <= f.n && d[go] == 0x1F && d[go + 1] == 0x8B && d[go + 2] == 8) {
        int flags = d[go + 3];
        size_t p = go + 10;
        if (flags & 4) p += 2 + ((size_t)d[p] | ((size_t)d[p + 1] << 8));
        if (flags & 8) { while (p < f.n && d[p]) p++; p++; }
        if (flags & 16) { while (p < f.n && d[p]) p++; p++; }
        if (flags & 2) p += 2;
        if (p < f.n) raw = md_inflate_heap(d + p, f.n - p, raw_n, 0);
    }
    mb_free(&f);
    if (!raw) md_seterr(err, errlen, "flujo gzip dañado en %s", path);
    return raw;
}

typedef struct {
    char title[256];
    int src_lang, tgt_lang;
    const char *def_cs, *src_cs, *tgt_cs;
    int utf8;
} bgl_info;

static void read_info(const uint8_t *raw, size_t n, bgl_info *in, mbuf *title_raw)
{
    memset(in, 0, sizeof *in);
    in->src_lang = in->tgt_lang = -1;
    size_t p = 0;
    while (p < n) {
        int b = raw[p++];
        int type = b & 0xF;
        size_t len = (size_t)(b >> 4);
        if (len < 4) {
            if (p + len + 1 > n) break;
            len = be(raw + p, (int)len + 1);
            p += (b >> 4) + 1;
        } else len -= 4;
        if (p + len > n) len = n - p;
        const uint8_t *d = raw + p;
        p += len;
        if (!len) continue;
        if (type == 0 && d[0] == 8 && len > 1 && d[1] >= 0x41 && d[1] <= 0x4E) in->def_cs = bgl_charsets[d[1] - 0x41];
        if (type != 3 || len < 3) continue;
        uint32_t code = be(d, 2);
        const uint8_t *v = d + 2;
        size_t vn = len - 2;
        switch (code) {
        case 0x01: title_raw->n = 0; mb_put(title_raw, v, vn); break;
        case 0x07: case 0x08: {
            uint32_t lc = be(v, vn > 4 ? 4 : (int)vn);
            int ok = lc < sizeof bgl_langs / sizeof *bgl_langs && bgl_langs[lc].enc;
            if (code == 7) in->src_lang = ok ? (int)lc : -1; else in->tgt_lang = ok ? (int)lc : -1;
            break;
        }
        case 0x11: in->utf8 = (be(v, vn > 4 ? 4 : (int)vn) & 0x8000) != 0; break;
        case 0x1A: if (v[0] >= 0x41 && v[0] <= 0x4E) in->src_cs = bgl_charsets[v[0] - 0x41]; break;
        case 0x1B: if (v[0] >= 0x41 && v[0] <= 0x4E) in->tgt_cs = bgl_charsets[v[0] - 0x41]; break;
        }
    }
}

static void detect_enc(const bgl_info *in, char *src, char *tgt)
{
    const char *def = in->def_cs ? in->def_cs : "cp1252";
    const char *s = in->utf8 ? "utf-8" : in->src_cs ? in->src_cs : in->src_lang >= 0 ? bgl_langs[in->src_lang].enc : def;
    const char *t = in->utf8 ? "utf-8" : in->tgt_cs ? in->tgt_cs : in->tgt_lang >= 0 ? bgl_langs[in->tgt_lang].enc : def;
    snprintf(src, 16, "%s", s);
    snprintf(tgt, 16, "%s", t);
}

static int build(bgl *g, md_dict *d, const char *ip, const char *dp, char *err, int errlen)
{
    size_t n = 0;
    uint8_t *raw = inflate_bgl(d->path, &n, err, errlen);
    if (!raw) return -1;
    bgl_info in;
    mbuf traw = {0}, w = {0}, title = {0};
    read_info(raw, n, &in, &traw);
    detect_enc(&in, g->src_enc, g->tgt_enc);
    bgl_decode(g->tgt_enc, (uint8_t *)traw.p, traw.n, 1, &title);
    mb_cstr(&title);
    strip_py(&title, 1, 1);
    while (title.n && title.p[title.n - 1] == 0) title.n--;
    mb_cstr(&title);

    /* copia en trozos LZ4 */
    struct stat st;
    stat(d->path, &st);
    uint32_t nch = (uint32_t)((n + DAT_CHUNK - 1) / DAT_CHUNK);
    FILE *f = fopen(dp, "wb");
    int rc = -1;
    if (!f) { md_seterr(err, errlen, "no se puede escribir %s", dp); goto out; }
    uint8_t h[DAT_HDR] = {0};
    memcpy(h, DAT_MAGIC, 8);
    put64(h + 8, (uint64_t)st.st_size);
    put64(h + 16, (uint64_t)st.st_mtime);
    put64(h + 24, n);
    h[32] = (uint8_t)nch; h[33] = (uint8_t)(nch >> 8); h[34] = (uint8_t)(nch >> 16); h[35] = (uint8_t)(nch >> 24);
    memcpy(h + 36, g->src_enc, 16);
    memcpy(h + 52, g->tgt_enc, 16);
    memcpy(h, "XXXXXXXX", 8); /* el magic se escribe al final: así un archivo a medias no vale */
    uint64_t *offs = calloc(nch + 1, 8);
    uint8_t *tab = calloc(nch + 1, 8);
    char *cb = malloc((size_t)LZ4_compressBound(DAT_CHUNK));
    int wok = offs && tab && cb && fwrite(h, 1, DAT_HDR, f) == DAT_HDR && fwrite(tab, 8, nch + 1, f) == nch + 1;
    uint64_t pos = DAT_HDR + (uint64_t)(nch + 1) * 8;
    for (uint32_t i = 0; wok && i < nch; i++) {
        size_t cl = n - (size_t)i * DAT_CHUNK < DAT_CHUNK ? n - (size_t)i * DAT_CHUNK : DAT_CHUNK;
        int c = LZ4_compress_default((const char *)raw + (size_t)i * DAT_CHUNK, cb, (int)cl, LZ4_compressBound(DAT_CHUNK));
        if (c <= 0 || fwrite(cb, 1, (size_t)c, f) != (size_t)c) wok = 0;
        offs[i] = pos;
        pos += (uint64_t)c;
    }
    offs[nch] = pos;
    if (wok) {
        for (uint32_t i = 0; i <= nch; i++) put64(tab + 8 * i, offs[i]);
        wok = !fseeko(f, DAT_HDR, SEEK_SET) && fwrite(tab, 8, nch + 1, f) == nch + 1 &&
              !fseeko(f, 0, SEEK_SET) && fwrite(DAT_MAGIC, 1, 8, f) == 8;
    }
    free(offs); free(tab); free(cb);
    if (fclose(f) || !wok) { md_seterr(err, errlen, "no se puede escribir %s", dp); remove(dp); goto out; }

    /* índice: palabra y alternativas -> (inicio, longitud) del bloque */
    sidx_builder *bld = sidx_new();
    bgl_entry *e = malloc(sizeof *e);
    size_t p = 0;
    while (p < n) {
        int b = raw[p++];
        int type = b & 0xF;
        size_t len = (size_t)(b >> 4);
        if (len < 4) {
            if (p + len + 1 > n) break;
            len = be(raw + p, (int)len + 1);
            p += (b >> 4) + 1;
        } else len -= 4;
        if (p + len > n) len = n - p;
        size_t start = p;
        p += len;
        if (!len || !(type == 1 || type == 7 || type == 10 || type == 11 || type == 13)) continue;
        if (parse_entry(type, raw + start, len, e)) continue;
        uint64_t off = ((uint64_t)start << 4) | (uint64_t)type;
        process_key(g, e->word, e->word_n, 0, &w);
        mbuf head = {0};
        mb_put(&head, w.p, w.n);
        if (w.n) sidx_add(bld, w.p, w.n, off, (uint32_t)len);
        for (int i = 0; i < e->nalts; i++) {
            process_key(g, e->alts[i], e->alts_n[i], 1, &w);
            if (w.n && !(w.n == head.n && !memcmp(w.p, head.p, w.n))) sidx_add(bld, w.p, w.n, off, (uint32_t)len);
        }
        mb_free(&head);
    }
    free(e);
    const char *lf = in.src_lang >= 0 && *bgl_langs[in.src_lang].iso ? bgl_langs[in.src_lang].iso : NULL;
    const char *lt = in.tgt_lang >= 0 && *bgl_langs[in.tgt_lang].iso ? bgl_langs[in.tgt_lang].iso : NULL;
    rc = sidx_write(bld, ip, d->path, title.n ? title.p : d->name, lf, lt, err, errlen);
    sidx_free(bld);
out:
    free(raw);
    mb_free(&traw); mb_free(&w); mb_free(&title);
    return rc;
}

static int bgl_needs_index(const char *path, const char *cache_dir)
{
    for (int k = 0; k < 2; k++) {
        char *ip = sidx_path_for(path, cache_dir, k);
        char *dp = dat_path(ip);
        int fresh = sidx_is_fresh(ip, path) && dat_fresh(dp, path);
        free(ip); free(dp);
        if (fresh) return 0;
    }
    return 1;
}

static void bgl_free(bgl *g)
{
    if (!g) return;
    sidx_close(g->idx);
    if (g->dat) fclose(g->dat);
    free(g->choff);
    free(g->cbuf);
    free(g);
}

static int open_dat(bgl *g, const char *dp)
{
    g->dat = fopen(dp, "rb");
    if (!g->dat) return -1;
    uint8_t h[DAT_HDR];
    if (fread(h, 1, DAT_HDR, g->dat) != DAT_HDR || memcmp(h, DAT_MAGIC, 8)) return -1;
    g->raw_size = get64(h + 24);
    g->nchunks = (uint32_t)h[32] | ((uint32_t)h[33] << 8) | ((uint32_t)h[34] << 16) | ((uint32_t)h[35] << 24);
    memcpy(g->src_enc, h + 36, 16); g->src_enc[15] = 0;
    memcpy(g->tgt_enc, h + 52, 16); g->tgt_enc[15] = 0;
    uint8_t *tab = malloc((size_t)(g->nchunks + 1) * 8);
    g->choff = malloc((size_t)(g->nchunks + 1) * 8);
    if (!tab || !g->choff || fread(tab, 8, g->nchunks + 1, g->dat) != g->nchunks + 1) { free(tab); return -1; }
    for (uint32_t i = 0; i <= g->nchunks; i++) g->choff[i] = get64(tab + 8 * i);
    free(tab);
    g->cbuf = malloc((size_t)LZ4_compressBound(DAT_CHUNK));
    g->cchunk = -1;
    return g->cbuf ? 0 : -1;
}

static int bgl_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    bgl *g = calloc(1, sizeof *g);
    char *ip = NULL, *dp = NULL;
    for (int k = 0; k < 2 && !ip; k++) {
        char *i2 = sidx_path_for(d->path, cache_dir, k);
        char *d2 = dat_path(i2);
        if (sidx_is_fresh(i2, d->path) && dat_fresh(d2, d->path)) { ip = i2; dp = d2; }
        else { free(i2); free(d2); }
    }
    if (!ip) {
        ip = sidx_path_for(d->path, cache_dir, 1);
        dp = dat_path(ip);
        if (build(g, d, ip, dp, err, errlen)) goto fail;
    }
    if (open_dat(g, dp)) { md_seterr(err, errlen, "no se puede leer %s", dp); goto fail; }
    char *name = NULL, *lf = NULL, *lt = NULL;
    g->idx = sidx_open(ip, &name, &lf, &lt, &d->word_count, err, errlen);
    if (!g->idx) goto fail;
    if (name && *name) { free(d->name); d->name = name; } else free(name);
    d->lang_from = lf;
    d->lang_to = lt;
    free(ip); free(dp);
    d->is_html = 1;
    d->priv = g;
    return 0;
fail:
    free(ip); free(dp);
    bgl_free(g);
    return -1;
}

static void bgl_close(md_dict *d) { bgl_free(d->priv); }

static int load_chunk(bgl *g, uint32_t i)
{
    if ((long)i == g->cchunk) return 0;
    if (i >= g->nchunks) return -1;
    size_t cl = (size_t)(g->choff[i + 1] - g->choff[i]);
    if (cl > (size_t)LZ4_compressBound(DAT_CHUNK) || fseeko(g->dat, (off_t)g->choff[i], SEEK_SET) ||
        fread(g->cbuf, 1, cl, g->dat) != cl) return -1;
    int r = LZ4_decompress_safe((const char *)g->cbuf, (char *)g->chunk, (int)cl, DAT_CHUNK);
    if (r < 0) return -1;
    g->chunk_len = (size_t)r;
    g->cchunk = (long)i;
    return 0;
}

static int read_raw(bgl *g, uint64_t off, size_t len, mbuf *out)
{
    out->n = 0;
    if (off + len > g->raw_size || mb_reserve(out, len)) return -1;
    while (out->n < len) {
        uint64_t o = off + out->n;
        if (load_chunk(g, (uint32_t)(o / DAT_CHUNK))) return -1;
        size_t in = (size_t)(o % DAT_CHUNK);
        if (in >= g->chunk_len) return -1;
        size_t take = g->chunk_len - in;
        if (take > len - out->n) take = len - out->n;
        memcpy(out->p + out->n, g->chunk + in, take);
        out->n += take;
    }
    return 0;
}

static int bgl_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    bgl *g = d->priv;
    sidx_hit hits[64];
    int n = sidx_find(g->idx, word, fuzzy, hits, max < 64 ? max : 64);
    mbuf blk = {0}, html = {0}, head = {0};
    bgl_entry *e = malloc(sizeof *e);
    for (int i = 0; i < n; i++) {
        int type = (int)(hits[i].off & 0xF);
        if (e && !read_raw(g, hits[i].off >> 4, hits[i].len, &blk) &&
            !parse_entry(type, (uint8_t *)blk.p, blk.n, e)) {
            process_key(g, e->word, e->word_n, 0, &head);
            process_defi(g, e->defi, e->defi_n, &html);
            const char *hw = head.n ? head.p : hits[i].word;
            if (!md_results_has(r, hw, html.p)) md_results_add(r, hw, strlen(hw), html.p, html.n);
        }
        free(hits[i].word);
    }
    free(e);
    mb_free(&blk); mb_free(&html); mb_free(&head);
    return r->n;
}

const md_format_t md_fmt_bgl = {"babylon", bgl_probe, bgl_needs_index, bgl_open, bgl_close, bgl_lookup};
