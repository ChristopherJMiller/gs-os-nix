# Regular expressions: builtins.match and builtins.split

Nix 2.34 compiles a pattern with `std::regex(re, std::regex::extended)` and
runs `std::regex_match` (`match`) or a `std::cregex_iterator` (`split`) over
the string, in the C++ "C" locale (Nix never sets a global one). So every
answer is libstdc++'s — the one the host's Nix links, GCC 15.3's — and
libstdc++'s answers are not POSIX's. Its executor is a backtracking search
with rules of its own, and they decide which match is found, which groups
capture what, and which patterns are errors.

So the VM's engine, `target/vm/c/regex.c`, is a **port** of libstdc++'s
regex, structure for structure: `bits/regex_scanner.tcc`,
`regex_compiler.tcc` and `regex_automaton.tcc` build the same NFA (dummies,
clones and orphaned states included), and the executor is the depth-first
half of `regex_executor.tcc`. That is the one both entry points run:
`__regex_algo_impl` picks the breadth-first executor only for the
`__polynomial` flag, which Nix does not pass. `regex.c` is derived from the
GNU ISO C++ Library (Copyright (C) 2010-2025 Free Software Foundation, Inc.,
GPL-3.0-or-later with the GCC Runtime Library Exception 3.1), which this
GPL-3.0-or-later project may include. Its notice is with the code.

`match.c` holds the two primops (`$4C match`, `$4D split`, BYTECODE.md).

## The rules, measured

Each was measured on the host first, and each is an answer in
`tests/fixtures/eval-okay-regex-rules.nix`. An error is one of the
`eval-fail-regex-*` fixtures.

**The search.** These are the executor's rules (`_M_dfs` and its handlers).

- An alternation tries both arms, the left first (`_M_handle_alternative`,
  POSIX branch). In a search the longest solution is kept, and of two as
  long, the first (`_M_handle_accept`: `<`, not `<=`). So `split "(a|ab)"
  "abc"` is `[ "" [ "ab" ] "c" ]`, and `split "(a|ab)(b?)" "ab"` keeps
  `[ "a" "b" ]`.
- A whole-string `match` keeps the **first** full match the search order
  meets. All have the same length, so none replaces it: `match
  "(a|ab)(c|bcd)(d*)" "abcd"` is `[ "a" "bcd" "" ]`.
- A greedy loop tries one more iteration, and then its exit **only if
  nothing has matched yet** (`_M_handle_repeat`: `if (!_M_has_sol)`). A loop
  that found a match never tries its earlier exits. So `split "(a*)(ab)?"
  "aab"` takes `"aa"` and leaves `(ab)` null, where POSIX's leftmost-longest
  would take all of "aab".
- A loop may be entered again at the place it was entered (`_M_rep_count`:
  twice per position). It is invisible unless an empty iteration captures
  something no later one overwrites: `match "((^)|a)*" "a"` is `[ "a" "" ]`,
  the `(^)` of the iteration before the one that took the `a`. Entered
  once, the loop would exit there, and that capture would be undone.
- A group's capture is never reset between iterations: `match "((a)|b)*"
  "ab"` is `[ "b" "a" ]`. Each step is undone exactly when it is backed out
  of (`match "(a)b|ac" "ac"` is `[ null ]`). A group that took no part is
  `null`.
- `^` matches only where the string starts. A later search has
  `match_prev_avail`, so `split "^a" "aaa"` splits once. `$` matches at the
  end. Neither is multiline (that is ECMAScript's).
- `split` is `regex_iterator::operator++`. After an empty match it searches
  the same place again, non-empty and anchored (`match_not_null |
  match_continuous`, without `match_prev_avail` until the first such retry
  fails), and then from one byte on. So `split "(x*)" "axb"` is `[ "" [ ""
  ] "a" [ "x" ] "" [ "" ] "b" [ "" ] "" ]`.

**The syntax.** These are the scanner's and compiler's rules, ERE as
libstdc++ reads it.

- The special characters are `. [ \ ( ) * + ? { | ^ $`; `]` and `}` are
  ordinary.
- Nix is built with `__STRICT_ANSI__`, so only a special character may be
  escaped: `\.` and `\\` are characters, and `\d`, `\]` and `\n` are errors.
  In a bracket a backslash is itself.
- In a bracket, a `]` first (after the `^`) is a character, and so is a `-`
  first or last. A `-` after a range or a class that does not end the
  bracket is an error (`[a-z-9]`). A range runs over `char`, which is
  **signed**: `[a-é]` (é is $C3 $A9) runs backwards and is an error, and
  `[^a-z]` matches each byte of é.
- Classes are the "C" locale's, and are ASCII only. Their names are
  lowercased first (`[[:ALPHA:]]`), and `w`, `d` and `s` are among them (`w`
  is alnum and `_`). An equivalence class `[=x=]` is `tolower`'s. A collating
  element `[.name.]` is one of the 128 names libstdc++ lists ("hyphen",
  "space", "a", ...): "1" is not one, and neither is "-".
- `*?` is two quantifiers, not a lazy one (lazy is ECMAScript's). `a**` is
  valid. A quantifier with nothing before it is an error (`*a`, `^*`).
- `{m}`, `{m,}`, `{m,n}`: the atom cloned m times, then a loop or n − m
  optional copies. A clone shares the group's number, so `match "(a){2}" "aa"`
  is `[ "a" ]`. A count past `INT_MAX` is an error, as is `{n,m}` with m < n,
  or `{,n}`.
- An empty alternative (`a|`) and an empty group (`()`) are valid.

**The values.** These come from Nix's primops.

- The pattern is forced first, as a string without context
  (`forceStringNoCtx`: E_TYPE otherwise), and **compiled before the string is
  forced**. So an invalid pattern is the error even when the string would
  throw. "invalid regular expression" is not an error `tryEval` catches, and
  E_REGEX is not either.
- The string must be a string (`forceString`: a path is E_TYPE). Its
  context is dropped: no piece of a result has any. But `split` with no match
  is `[ s ]`, the argument itself, so that one keeps it.
- A pattern Nix cannot build (past libstdc++'s 100,000 NFA states, "memory
  limit exceeded by regular expression") is E_NOMEM here, and so is one past
  the bank's own limit (below).

## The port

**The NFA.** It is built in **bank 7**, the regex engine's own (VM.md). The
states are 8 bytes each (opcode, next, and one word that is the alt, the
byte, the bitmap or the group), at most 7,916 less what the bitmaps and
captures take. A bracket is compiled
once to a 32-byte bitmap of the 256 bytes, which is exact: `_BracketMatcher`
is a pure function of the byte, and its cache is the same function. The
parser's recursion runs only through groups, and here it is a stack of 128
frames; a deeper nesting is E_NOMEM. A clone keeps its map in the states'
spare word, so it needs no `std::map`.

**The executor.** libstdc++ recurses once per step, and so as deep as the
match is long, which the 65816's 7 KB C stack could not hold. Here every
recursive call that has something to do afterwards pushes a record of it,
undone when the search backs out:

| Record | Bytes | Afterwards |
|---|---|---|
| `F_MATCH` | 1 | `--current` |
| `F_SB` | 5 | group k's start back |
| `F_SE` | 6 | group k's end and matched flag back |
| `F_REP` | 6 | the loop's repeat count back, then its exit unless matched |
| `F_REPDEC` | 3 | the count less one, then the exit unless matched |
| `F_ALT` | 3 | the other arm (`has_sol` saved, then cleared) |
| `F_ALTT`, `F_ALTF` | 1 | `has_sol \|=` the saved value |

`.*` costs 7 bytes a byte of the string, and `(.)*` 18. The stack shares
bank 7 with the NFA and the matches, so a search deeper than that is E_NOMEM.

**Memory and the collector.** Nothing in `regex.c` allocates or runs
bytecode. Positions are 16-bit offsets, and the pattern and the string are
views the caller holds. `match.c` checks the pattern and compiles it, forces
the string, and compiles the pattern **again**: forcing can run a `match` of
its own, which reuses bank 7 (the fixture's `inner`). Then it searches,
leaving one record per match in bank 7, which no collection touches. It
reserves the whole result in one `nursery_reserve`, so that building it
cannot collect, views the string again, and builds the lists. A result
larger than a bank (or than the nursery can free) is E_NOMEM.

**Cost.** The eleven answers of a first test took 1.2M cycles on a2min,
about 110K cycles (0.04 s) each. The search is exponential exactly where
libstdc++'s is (the fuzzer's `(.?|)?` inside a `+`): such a pattern hangs Nix
too.

## How it was checked

- **Natively, against libstdc++ itself.** A C++ harness built with GCC 15.3
  (the dev shell's compiler) runs `std::regex` exactly as `prim_match` and
  `prim_split` do. `regex.c` is built natively too, with `RX_HOST` and a
  64 KB buffer for bank 7. Both print the positions of every group, which is
  stricter than the strings Nix shows. Results: 320,000 random cases agree,
  structured patterns and random metacharacter soup with subjects up to 25
  bytes, errors and all (the 13 cases that ran past a second ran past it on
  both sides). So do 12,356 exhaustive ones: every class name against every
  byte, all 128 collating names as `[. .]` and `[= =]`, a backslash before
  every byte, every byte in a bracket, and ranges across 128.
- **On the VM, against the host.** The fixtures above, in every GC mode.
- **Mutations.** Each rule broken in a copy, and the fixtures catch it
  (`scripts/mutation-vm.sh`).
