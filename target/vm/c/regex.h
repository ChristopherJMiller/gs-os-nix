/* regex.h -- std::regex as Nix 2.34 runs it, for builtins.match and
 * builtins.split (regex.c, docs/REGEX.md). No VM types: regex.c builds on
 * its own, on the host as on the 65816. */
#ifndef REGEX_H
#define REGEX_H

#include <stdint.h>

/* Bank 7 (VM.md): the compiled NFA, the captures, split's matches and the
 * executor's stack. A primop's own until it returns: nothing here survives a
 * call that runs bytecode, which could compile a pattern of its own. */
#define REGEX_BASE  0x00070000UL

#define RX_EINVAL   1   /* not a regular expression: Nix's "invalid regular expression" */
#define RX_ESPACE   2   /* the bank is full: E_NOMEM (Nix's limit is 100,000 NFA states) */
_Noreturn void rx_fail(uint8_t why);    /* the caller's; does not return */

/* std::regex(re, n, std::regex::extended): rx_fail unless re is valid. */
void     rx_compile(const char *re, uint16_t n);
/* sub_count: group 0, the whole match, and one per '(' */
uint16_t rx_groups(void);
/* std::regex_match: 1 if all of s matches, its groups then record 0. */
int      rx_match(const char *s, uint16_t n);
/* std::cregex_iterator over s, as prim_split walks it: the number of
 * matches, each a record, in order. */
uint16_t rx_split(const char *s, uint16_t n);

/* Records: the groups of one match, rx_recsize() bytes apart from
 * rx_records(). Group k of the record at off: */
uint16_t rx_records(void);
uint16_t rx_recsize(void);
uint16_t rx_first(uint16_t off, uint16_t k);
uint16_t rx_second(uint16_t off, uint16_t k);
uint8_t  rx_matched(uint16_t off, uint16_t k);

#endif
