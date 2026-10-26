# Floats (C1, normative)

Nix floats are IEEE-754 binary64 (`NixFloat` is `double`), and the VM computes
them **bit for bit as the host does**, in software (decided with Chris,
2026-10-26: soft-float in C, not ORCA/C's SANE `double`, whose Toolbox lives in
the IIgs ROM a2min does not have and whose extended intermediates round
differently). The card never sees a float. Rules from Nix 2.34.9
(`primops.cc`, `eval.cc`, `print.cc`), each measured on the host.

## Values

| | |
|---|---|
| heap | `FLOAT`, tag `$0D`, no `aux`; payload 8 bytes, the IEEE-754 binary64 bits little-endian (HEAP.md) -- an `INT`'s shape |
| constant | kind `$0D` (kinds are tags): 8 bytes, the same (BYTECODE.md "Constants") |
| literal | `(([1-9][0-9]*\.[0-9]*)\|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?`, parsed correctly rounded: the generator's Rust `f64` parse, as Nix's `strtod`. Nix calls the literal `invalid float`, a parse error for the whole file, when `strtod` sets `errno`, and the generator refuses it the same way (`front.rs` `float_literal`): on overflow (`1.7976931348623159e308`; `…158e308` is the largest finite), and on underflow, which glibc reports when the result **rounded to 53 bits with an unbounded exponent** is below 2^-1022. That is every nonzero literal below 2^-1022 − 2^-1076, the midpoint under the smallest normal, including one that rounds to it: measured, `2.2250738585072012e-308` is invalid and `2.2250738585072013e-308` valid, though both are below 2^-1022 = `2.2250738585072013830…e-308`; the midpoint itself, written out in its 1,078 digits, is valid, and one unit in its last digit less is not. A zero is any zero: `0.0e-400` is `0` |

## Arithmetic

`+ - * /` (and `builtins.add`, `sub`, `mul`, `div`): **if either operand is a
float, both become floats and the result is a float**, rounded to nearest, ties
to even; otherwise integer arithmetic, checked as before (`E_OVERFLOW`). An
`INT` becomes a float rounded to nearest (C++'s conversion). Float arithmetic
never overflows into an error: it reaches infinity, and `inf - inf` is a NaN.
**Division by zero is `E_DIVZERO` for floats too** (`prim_div` tests the divisor
as a float). Unary minus is `0 - x`, so `-(0.0)` is `0.0`, not `-0.0` (a
negative zero arises only from a product or quotient).

**NaN is x86's:** the invalid operations (`inf - inf`, `0 * inf`, `inf / inf`)
give the default NaN, `0xFFF8000000000000` -- sign set, so the host prints it
`-nan` -- and a NaN operand comes out unchanged (quieted). Every NaN a Nix
program can make is that one, so which operand wins does not show. Measured:
`(1.0e308 * 10) - (1.0e308 * 10)` is `-nan`, and so is every operation on it.

`<` and `==` compare an `INT` with a float as doubles (C++'s usual arithmetic
conversions), so `1 == 1.0` and `[ 1 ] == [ 1.0 ]`; a NaN is unequal to
everything, itself included, and unordered (`<` is false both ways, so
`nan <= 1` and `nan >= 1`, being `!(1 < nan)` and `!(nan < 1)`, are true).
A float and a string are neither equal nor ordered (`1.5 < "a"` is an error).

**Which NaNs are the same value.** Nix compares list elements and attribute
values by pointer before by value, so `[ nan ] == [ nan ]` is true exactly when
both elements name one `Value` -- a variable or an attribute that is one --
while at the top level `nan == nan` compares two fresh values and is false.
The VM copies a forced float into its thunk (`run.c` `force_thunk`), a new
object only where Nix also has one: measured on twelve ways of naming a NaN
(`eval-okay-float-identity`), from `let y = nan` (shared) to `{ a = nan; b =
s.a; }` (not), the VM answers as the host. So `OP_EQ` on a float never takes
the pointer shortcut, and `values_equal` inside a value does. `builtins.floor` and `builtins.ceil`
take a float or an `INT` and return an `INT`: the result must lie in
[-2^63, 2^63) (`E_TYPE` otherwise), and an `INT` argument that did not survive
the round trip through a double is an error too (Nix's own guard, issue 12899).

## Text

| where | format | examples |
|---|---|---|
| `--eval`'s printer (a2min, the harness) | C++ `ostream << double`: `%g`, 6 significant digits | `1`, `0.333333`, `1.23457e+06`, `1e-05`, `inf`, `-0`, `-nan` |
| `toString` (the VM: a STRING value) | `std::to_string`: `%f`, 6 decimals, exact | `"0.100000"`, `"4.200000"`, `"-0.000000"`, `"-nan"`, `"1000000000000000019884624838656.000000"`; `0.0000005` is `"0.000000"` and `0.0000015` `"0.000002"`, each the exact binary value rounded |
| `toJSON` (the VM) | nlohmann/json's `dump`: its `to_chars`, Grisu2, then fixed point for 10^-4 <= x < 10^15, else `d.ddde+NN`; an infinity or a NaN `null` | `1.0`, `0.1`, `0.30000000000000004`, `1e+15`, `1e-05`, `0.0001`, `123456789.0`, `-0.0`, `1.7976931348623157e+308` |

The printer is the harness's, so a2min prints with the host's own `printf`. A
`toString` is the VM's to compute, and exactly: the decimal expansion of a
double is finite, and `%f` rounds it to 6 places, ties to even on the exact
value, so `float.c` works on the integer `M * 2^E` with a bignum of 16-bit
limbs (a double's integer part has at most 309 digits). `"${x}"` of a float is
an error, as in Nix (only `toString`'s wider rules coerce one).

**toJSON's digits are Grisu2's, not the shortest**: Grisu2 always round-trips
but in rare cases gives more digits than the shortest text that does, and
Nix prints what nlohmann's Grisu2 gives. So `float.c` `f_json` ports
nlohmann's `to_chars.hpp` (3.12.0, MIT; the notice is with the code) step for
step: the boundaries, the cached power of ten (its 79-entry table a string
literal, which stays with the code -- a static table would be bank 0's), the
digit generation and its rounding, and `format_buffer`'s layout.
`checks.float-vectors` holds it to nlohmann itself: `scripts/json-floats.cpp`
answers 400 vectors per seed with `nlohmann::json(x).dump()`, and a planted
bug (Grisu2 without its rounding step) must be caught, as it is, by 142.

`typeOf` is `"float"`; `builtins.isFloat`.

## Where

Two files in `segment "float"`, in bank 6 since `prims` filled bank 4
(omf2bin places a segment in bank 4 while it fits, then bank 6):

- `float.c` is the arithmetic on bit patterns and nothing else -- no VM in it,
  so `checks.float-vectors` builds and tests it alone. Every operation uses
  explicit-width integer arithmetic only (ORCA/C's `int` is 16 bits, so
  nothing relies on an integer promotion), and runs on a2min against vectors
  whose answers the host's own IEEE arithmetic and `%f` gave: random operands
  and every class of edge (zeros of both signs, subnormals, the largest finite,
  infinities, NaN, exact ties for rounding).
- `numbers.c` is the VM's side: `FLOAT` objects, the mixed `INT`/float tests
  (`num_pair`, `num_cmp`), `floor`/`ceil`, `toString`. Bank 0 keeps only a
  tag test in each operation that can see a float.

Bank 0 cost (TOOLCHAIN.md, scripts/bank-gate.py): ORCA/C links `~SHL8` and
`~LSHR8`, 46 bytes each, for **any** 64-bit shift, by a constant too
(measured: `a >> 52` alone links `~LSHR8`), and a float is nothing but 64-bit
shifts, so both are in. A *variable* 64-bit shift, a variable 16-bit shift
(`~SHIFTLEFT`, `~USHIFTRIGHT`) and a 32-bit multiply (`~UMUL4`) are not:
`float.c` shifts through 32-bit halves (`shl`, `shr`) and multiplies the
bignum by 1000 as `(x << 10) - (x << 4) - (x << 3)`. ORCA/C's optimizer
miscompiles two 64-bit `?:` shapes; `float.c` uses `if`s, and
`scripts/hostlint/orcabugs.py` refuses the shapes (TOOLCHAIN.md "Known
miscompiles").
