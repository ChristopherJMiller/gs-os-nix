# GS/OS: files and the environment

The builtins that read files or the environment ask GS/OS. That makes them
L7's first step, taken in simulation: the VM makes the calls a GS/OS
application makes, and a2min answers them from a host directory. Until a
program runs where GS/OS answers, each of these builtins is still `E_GSOS`
by name (D26). The decisions are D30's.

| Builtin | Nix 2.34 (`primops.cc`) | Here |
|---|---|---|
| `pathExists p` | `realisePath(p, Ancestors)`, or `Full` when `p` is a string ending in `/` or `/.` (it must then be a directory); `maybeLstat` | the same resolution, then `GetFileInfo` |
| `readFile p` | `realisePath(p, Full)`, the bytes; a NUL is an error | `Open`, `GetEOF`, `Read`, `Close` into a fresh string |
| `readFileType p` | `realisePath(p)` with **no** resolution, then `lstat`'s type -- and a link among the ancestors is an error | `GetFileInfo` of the path as written, its ancestors checked |
| `readDir p` | `realisePath(p, Full)`, each entry's type, `lstat`'s (no resolution) | `Open` the directory, `GetDirEntry` until `endOfDir` (on a file, GS/OS's error) |
| `hashFile t p` | the type forced and parsed first, then `readFile p` hashed | `hashString`'s code (`hashes.c`, the card for sha256) on what `readFile` reads |
| `getEnv s` | the process environment, `""` if unset | the ORCA shell's variables: `ReadVariable`, `""` if unset or no shell |

Everything else that needs GS/OS -- `path`, `filterSource`, `storePath`, the
fetchers, `getFlake` -- needs the store or the network and stays `E_GSOS`.

## Which argument is a path (`coerceToPath`)

A path value is used as it is. A set with `__toString` is called and its
result coerced again. Anything else is coerced to a string, which must begin
with `/` ("doesn't represent an absolute path", an error `tryEval` does not
catch: `E_TYPE`) and is then made canonical (`/a//b/./c/..` is `/a/b`). A
string with a context names store paths that Nix would build first: that
needs the store, so it is `E_GSOS` by name ("realise").

## Paths are GS/OS full pathnames

A Nix path is absolute and canonical, and GS/OS takes `/` as a pathname's
separator when the first separator it meets is `/`. So the path itself is the
GS/OS full pathname: `/a/b/c` is volume `a`, directory `b`, file `c`, and the
store's `/nix/store/...` is volume `nix`. Nothing is translated. Two paths
have no GS/OS name and are `E_GSOS` by name: `/` itself (GS/OS lists volumes
by a separate call, not as a directory), and a name with a `:` (every FST
refuses it). The GS/OS error from a call is `E_FILE` (`$14`, new): no such
file, volume or directory, or a file where a directory must be.

## Symlinks (D30): our FST's convention

GS/OS has no symlinks, and Nix stores are full of them. **A symlink is a
file of type `$F1` with aux type `$4C534E` ("NSL", little-endian in the
long word), whose contents are its target.** `$F1` is ProDOS's first
user-defined file type; the aux type is what tells this use of it from any
other. a2min reports every host symlink so, and the FST that holds the store
(L7) must store symlinks the same way.

GS/OS does not follow these, so the VM resolves them itself, exactly as
Nix's `SourceAccessor::resolveSymlinks` does: the path's components are
taken one at a time onto a result that starts at the root; `.` is skipped
and `..` pops; each other component is pushed, and if it is a symlink -- in
`Full` mode always, in `Ancestors` mode unless it is the last -- its target's
components are put back at the front of what remains, the result emptied
for an absolute target or popped for a relative one. 1,024 links are
allowed; the next is `E_FILE` ("infinite symlink recursion"). A component
that does not exist is pushed and the walk goes on: the call that uses the
result is the one that fails. With no resolution (`readFileType`), a link
before the last component is `E_FILE`: Nix's `PosixSourceAccessor` asserts
of every path it reads that no ancestor is a link ("path '...' is a
symlink", measured), and opens files `O_NOFOLLOW`; resolution is what
removes them first everywhere else.

A directory is storage type `$0D`, a symlink as above, and anything else a
regular file: GS/OS's hierarchy has nothing Nix would call `"unknown"`.

## `getEnv` (D30): the ORCA shell's variables

GS/OS has no environment; the ORCA shell (and GNO) keep variables, which a
program reads with the shell call `ReadVariable` (`$010B`), and those are
what `getEnv` reads. Under no shell the call is not answered and every
variable is `""`. One difference from Nix's: the ORCA shell folds the case of
a variable's name, so `TEST_VAR` and `test_var` are one variable.

## The calls, and where they are made

The VM makes GS/OS calls through the stack entry (`JSL $E100B0`, the call
number and the parameter block pushed), from a few lines of `asm` in
`gsos.c` -- not through ORCA/C's `<gsos.h>`, whose macros call ORCALib's
`PDosInt`, a library routine that would land in bank 0. The parameter
blocks are on the C stack; a file's bytes are read straight into the nursery
string that will hold them, so `readFile` copies nothing twice. The byte at
`$0334` (`GSOS_ADDR`, beside the harness flags) says whether GS/OS is there
to answer: 0, and each builtin is `E_GSOS` by name as before.

## a2min's GS/OS (simulation only)

a2min answers the calls the VM makes -- `GetFileInfo` (`$2006`), `Open`
(`$2010`), `Read` (`$2012`), `Close` (`$2014`), `GetEOF` (`$2019`),
`GetDirEntry` (`$201C`) and `ReadVariable` (`$010B`) -- from what it is told
to mount, and nothing else:

- `A2MIN_MOUNT=/pwd/lang=<dir>` mounts a host directory at a GS/OS path
  prefix. The scoreboard mounts each test's own directory at `/pwd/lang`,
  the fixture convention's (D7), so `./lib.nix` is the file beside the test.
  A path under no mount is `volNotFound`.
- `A2MIN_VAR_<NAME>=<value>` is the shell variable `NAME`. The scoreboard
  sets `TEST_VAR=foo`, as Nix's own `tests/functional/lang.sh` exports it
  for `eval-okay-getenv`.
- A host symlink is the convention's file, its target its contents, and is
  never followed by a2min itself.

It is not `orcarun`'s GS/OS, though that one answers the same calls for the
compiler: `orcarun` has one volume, the host root, follows host symlinks
silently, and models a shell for ORCA's tools. Here the volumes are exactly
the mounts, symlinks are the convention, and every call is read-only.

## How it was checked

- **The host's answers**, from `nix-instantiate` over `tests/fixtures/gsos/`:
  a file, a file with NULs, a directory, and links -- a chain, a relative
  target through `.` and `..`, one that climbs back through another link, a
  dangling one and a two-link loop. Five `eval-okay-gsos-*` fixtures (50
  answers), ten `eval-fail-gsos-*` and three refusals (a context, a `:`, the
  root), in every GC mode. Measuring found one rule that reading the code
  had missed: `readFileType`, which resolves nothing, does not let the
  kernel follow a link among the ancestors -- Nix refuses it ("path '...' is
  a symlink"), as every `PosixSourceAccessor` call does.
- **The corpus's nine**: `pathExists` 3, `readFile` 2, `readDir`,
  `readFileType` (both with symlinks), `hashFile` (all four algorithms, a
  file with NULs) and `getenv`.
- **The refusal stays tested**: a `refuse-gsos-*` fixture runs with nothing
  mounted (`scripts/scoreboard.py`, `mutation-vm.sh`), so D26's `E_GSOS` by
  name is still what a VM without GS/OS answers.
- **Mutations**: 15, one a rule (`scripts/mutation-vm.sh`). An explicit
  "not a directory" check in `readDir` was taken out rather than kept: its
  mutation survived, since GS/OS's own `GetDirEntry` error already answers.
- **Not checked against the host**: an absolute link target, which would
  name a host path the VM's `/pwd/lang` cannot match. The walk's restart
  from the root is Nix's line for line, and untested.
