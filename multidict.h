/*
 * libmultidict - lectura directa de diccionarios para KOReader
 *
 * API pública (estable, pensada para LuaJIT FFI).
 * Todas las cadenas de entrada y salida son UTF-8.
 */
#ifndef MULTIDICT_H
#define MULTIDICT_H

#ifdef __cplusplus
extern "C" {
#endif

#define MD_API __attribute__((visibility("default")))

typedef struct md_dict md_dict;
typedef struct md_results md_results;

/* Versión de la librería, p. ej. "1.0.0". */
MD_API const char *md_version(void);

/* Devuelve 1 si `path` parece un diccionario soportado por la librería
 * (según extensión y contenido mínimo), 0 en otro caso. */
MD_API int md_probe(const char *path);

/* Devuelve 1 si abrir el diccionario requerirá construir antes el índice
 * auxiliar (formatos sin índice propio: DSL, XDXF, TSV). Útil para mostrar
 * un aviso de "indexando..." antes de llamar a md_open(). */
MD_API int md_needs_index(const char *path, const char *cache_dir);

/* Abre un diccionario. `cache_dir` puede ser NULL: entonces el índice
 * auxiliar (si hace falta) se guarda junto al diccionario, o en cache_dir
 * si el directorio del diccionario no es escribible.
 * En caso de error devuelve NULL y escribe el motivo en err. */
MD_API md_dict *md_open(const char *path, const char *cache_dir, char *err, int errlen);
MD_API void md_close(md_dict *d);

MD_API const char *md_name(md_dict *d);      /* nombre visible del diccionario */
MD_API const char *md_format(md_dict *d);    /* "mdx", "dictd", "dsl", "xdxf", "tab", "tangodict", "mdict-db", "kobo",
                                                  "stardict-xml", "csv", "quickdic", "slob", "zim", "sdic",
                                                  "appledict", "babylon", "epub", "mobi" */
MD_API int md_is_html(md_dict *d);           /* 1 si las definiciones son HTML */
MD_API const char *md_lang_from(md_dict *d); /* puede ser "" */
MD_API const char *md_lang_to(md_dict *d);   /* puede ser "" */
MD_API long md_word_count(md_dict *d);       /* -1 si se desconoce */

/* Busca `word`. fuzzy=0: coincidencia exacta (sin distinguir mayúsculas).
 * fuzzy=1: además devuelve entradas que empiezan por `word`.
 * max_results limita el número de resultados (<=0: 32). */
MD_API md_results *md_lookup(md_dict *d, const char *word, int fuzzy, int max_results);
MD_API int md_results_count(md_results *r);
MD_API const char *md_results_word(md_results *r, int i);
MD_API const char *md_results_definition(md_results *r, int i);
MD_API void md_results_free(md_results *r);

#ifdef __cplusplus
}
#endif
#endif
