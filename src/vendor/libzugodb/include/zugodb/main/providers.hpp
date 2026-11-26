/* providers.hpp -- the functions the engine gets from its host
 * (libzugodb Stage 11.1, #40; design L25 and L29).
 *
 * The engine implements no regular expressions and leaves some Unicode work
 * to its host. A host hands it one versioned C table per service, through
 * zugodb::Database::SetProviders(), once and before the first query:
 *
 *     static const zugodb_regex_v1 regex = {ZUGODB_REGEX_ABI_VERSION,
 *                                           sizeof(zugodb_regex_v1), ...};
 *     zugodb_providers p = {ZUGODB_PROVIDERS_ABI_VERSION,
 *                           sizeof(zugodb_providers), &regex, NULL};
 *     db.SetProviders(&p);
 *
 * This header is plain C: it includes <stddef.h> and <stdint.h> only, and
 * names no R and no other library. The tables are the engine's own; a host
 * whose libraries have tables of another shape (zugodb, over zuregex's
 * zuregex_api_v1 and zuutf's zuutf_api) writes small functions that adapt
 * them.
 *
 * Versions. Each table starts with abi_version and struct_size. A table is
 * used only when abi_version is the one below and struct_size is at least
 * the size of the table this header defines, and no member is NULL.
 * Otherwise the service is absent: its functions are refused with the error
 * they get when no table is given at all. Members are appended, never
 * reordered or removed; a change to an existing member bumps the ABI
 * version.
 *
 * The rules for every member:
 *   - It is safe to call from any thread at once, on distinct match states;
 *     a compiled pattern is read-only and shared by threads. (The engine
 *     calls providers on the query's thread today, design L29, but the
 *     contract is the one above.)
 *   - It never throws, unwinds, longjmps, prints, exits or calls back into
 *     the host's runtime (R): it returns a status. Every status but
 *     ZUGODB_PROVIDER_OK and ZUGODB_PROVIDER_NOMATCH is an error the engine
 *     turns into a query error, with the text strerror() gives.
 *   - Strings are a pointer and a length in bytes, not NUL-terminated.
 *   - strerror() returns a static string, or NULL.
 *
 * The regex table's semantics are DuckDB's, which are RE2's with DuckDB's
 * defaults: the engine cannot check that, so a host must not give a table
 * with another dialect's meaning (PCRE's, ECMAScript's). Until its regex
 * library has that dialect (zuregex#211), a host gives no regex table.
 */
#ifndef ZUGODB_PROVIDERS_H
#define ZUGODB_PROVIDERS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- statuses ----------------------------------------------------------- */

#define ZUGODB_PROVIDER_OK 0
#define ZUGODB_PROVIDER_NOMATCH 1               /* search: no match; group: did not take part */
#define ZUGODB_PROVIDER_ERR_LIMIT (-1)          /* a match, depth or memory limit was hit */
#define ZUGODB_PROVIDER_ERR_INVALID_UTF8 (-2)   /* the text or pattern is not UTF-8 */
#define ZUGODB_PROVIDER_ERR_BAD_PATTERN (-3)    /* a pattern the dialect refuses */
#define ZUGODB_PROVIDER_ERR_BAD_REWRITE (-4)    /* a malformed replacement */
#define ZUGODB_PROVIDER_ERR_BUFFER_TOO_SMALL (-5) /* *outlen is set to the size needed */
#define ZUGODB_PROVIDER_ERR_NOMEM (-6)
#define ZUGODB_PROVIDER_ERR_ARGUMENT (-7)       /* a NULL pointer, unknown bit or bad group */
#define ZUGODB_PROVIDER_ERR_OTHER (-8)

/* ---- regular expressions ------------------------------------------------ */

#define ZUGODB_REGEX_ABI_VERSION 1u

/* Opaque to the engine. */
typedef struct zugodb_regex_pattern zugodb_regex_pattern;
typedef struct zugodb_regex_match zugodb_regex_match;

/* compile() flags. */
#define ZUGODB_REGEX_CASELESS 0x1u /* DuckDB's option 'i' */

/* search() options. Without one, the pattern matches anywhere in the text. */
#define ZUGODB_REGEX_ANCHOR_BOTH 0x1u /* the match must be the whole text */

/* replace() options. Without one, only the first match is replaced. */
#define ZUGODB_REGEX_REPLACE_GLOBAL 0x1u /* every match (DuckDB's option 'g') */

typedef struct {
    uint32_t abi_version; /* ZUGODB_REGEX_ABI_VERSION */
    uint32_t struct_size; /* sizeof the provider's table */

    /* Compiles `pattern` with ZUGODB_REGEX_* flags. On ZUGODB_PROVIDER_OK,
     * *out is a pattern to free with pattern_free(). Otherwise *out is
     * NULL and, when errbuf is not NULL, a message is written there,
     * NUL-terminated and cut at errlen. */
    int (*compile)(const char *pattern, size_t len, uint32_t flags,
                   zugodb_regex_pattern **out, char *errbuf, size_t errlen);
    void (*pattern_free)(zugodb_regex_pattern *p);

    /* The capture groups of `p`, not counting group 0; or an error. */
    int (*group_count)(const zugodb_regex_pattern *p);

    /* Match state for `p`, used by one thread at a time. Free it with
     * match_free(). */
    int (*match_create)(const zugodb_regex_pattern *p, zugodb_regex_match **out);
    void (*match_free)(zugodb_regex_match *m);

    /* Searches `text` with search() options. ZUGODB_PROVIDER_OK on a match,
     * whose groups group() then reads; ZUGODB_PROVIDER_NOMATCH; or an
     * error, never NOMATCH, when a limit is hit. */
    int (*search)(const zugodb_regex_pattern *p, const char *text, size_t len,
                  uint32_t options, zugodb_regex_match *m);

    /* The byte offsets of group `group` (0 is the whole match) of the last
     * successful search() with `m`, half-open. ZUGODB_PROVIDER_NOMATCH for
     * a group that did not take part. */
    int (*group)(const zugodb_regex_match *m, int group, size_t *start, size_t *end);

    /* Replaces the first match of `p` in `text`, or every match with
     * ZUGODB_REGEX_REPLACE_GLOBAL, by `rewrite` in RE2's form (\0 to \9 a
     * group, \\ a backslash). With no match the result is the text. *outlen
     * is the capacity of `out` on entry; on ZUGODB_PROVIDER_OK it is the
     * result's length (no NUL is written); on
     * ZUGODB_PROVIDER_ERR_BUFFER_TOO_SMALL it is the capacity that is
     * enough. `m` is match state for `p`. */
    int (*replace)(const zugodb_regex_pattern *p, const char *text, size_t len,
                   const char *rewrite, size_t rlen, uint32_t options,
                   zugodb_regex_match *m, char *out, size_t *outlen);

    /* A message for a status, or NULL. */
    const char *(*strerror)(int status);
} zugodb_regex_v1;

/* ---- UTF-8 -------------------------------------------------------------- */

#define ZUGODB_UTF8_ABI_VERSION 1u

/* The engine counts code points itself, on any bytes. With this table, a
 * count of valid UTF-8 is the provider's; a count of invalid UTF-8 stays
 * the engine's, so the answer is the same with or without the table. */
typedef struct {
    uint32_t abi_version; /* ZUGODB_UTF8_ABI_VERSION */
    uint32_t struct_size; /* sizeof the provider's table */

    /* ZUGODB_PROVIDER_OK when `s` is valid UTF-8,
     * ZUGODB_PROVIDER_ERR_INVALID_UTF8 when it is not, or another error. */
    int (*validate)(const char *s, size_t n);

    /* The code points of `s`, which validate() accepted, in *out. */
    int (*count)(const char *s, size_t n, size_t *out);

    /* A message for a status, or NULL. */
    const char *(*strerror)(int status);
} zugodb_utf8_v1;

/* ---- the set ------------------------------------------------------------ */

#define ZUGODB_PROVIDERS_ABI_VERSION 1u

typedef struct {
    uint32_t abi_version; /* ZUGODB_PROVIDERS_ABI_VERSION */
    uint32_t struct_size; /* sizeof the host's zugodb_providers */

    const zugodb_regex_v1 *regex; /* NULL: the regex functions are refused */
    const zugodb_utf8_v1 *utf8;   /* NULL: the engine's own counts */

    /* Reserved for later services; the engine does not read them. */
    const void *uuid;
    const void *random;
    const void *decimal;
} zugodb_providers;

#ifdef __cplusplus
}
#endif

#endif /* ZUGODB_PROVIDERS_H */
