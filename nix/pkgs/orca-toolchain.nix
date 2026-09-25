# The C toolchain as ordinary commands (docs/TOOLCHAIN.md, D18).
#
#   orcacc   [-o NAME] [-I DIR]... [-O] [-d OUTDIR] FILE.c     -> NAME.root NAME.a
#   orcalink [-o OUT] [-d OUTDIR] NAME...                       -> OUT (OMF load file)
#   omf2bin  [--org N] IN OUT                                   -> flat binary for a2min
#   orcabuild -o OUT.bin [-O] [-I DIR]... FILE.c...             -> all three
#
# Each command stages its inputs into a scratch directory that orcarun presents to
# the program as GS/OS prefix 0, runs the real ORCA binary on it, and copies the
# results back out with the ProDOS `#TTAAAA` suffixes stripped. Source files are
# converted to CR line endings on the way in (ORCA/C's SRC files are CR-terminated).
{ lib
, writeShellApplication
, symlinkJoin
, orcac
, orcarun
}:

let
  orca = "${orcac}/orca";

  common = ''
    ORCA=${orca}
    RUN=${orcarun}/bin/orcarun
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
  '';

  orcacc = writeShellApplication {
    name = "orcacc";
    text = ''
      ${common}
      name=""; outdir="."; minus="ip"; plus=""; includes=(); src=""
      while [ $# -gt 0 ]; do
        case "$1" in
          -o) name=$2; shift 2 ;;
          -d) outdir=$2; shift 2 ;;
          -I) includes+=("$2"); shift 2 ;;
          -O) plus="''${plus}o"; shift ;;
          --minus) minus="''${minus}$2"; shift 2 ;;
          --plus) plus="''${plus}$2"; shift 2 ;;
          -*) echo "orcacc: unknown option $1" >&2; exit 2 ;;
          *) if [ -n "$src" ]; then echo "orcacc: one source file per call" >&2; exit 2; fi; src=$1; shift ;;
        esac
      done
      [ -n "$src" ] || { echo "usage: orcacc [-o NAME] [-I DIR]... [-O] [-d OUTDIR] FILE.c" >&2; exit 2; }
      base=$(basename "$src"); base=''${base%.*}
      [ -n "$name" ] || name=$base
      # ORCA/C reads CR-terminated SRC files ($B0, auxtype 8 = C).
      tr '\n' '\r' < "$src" > "$work/$base.c#B00008"
      srcdir=$(dirname "$src")
      for d in "$srcdir" "''${includes[@]}"; do
        for h in "$d"/*.h; do
          [ -f "$h" ] || continue
          hn=$(basename "$h")
          tr '\n' '\r' < "$h" > "$work/$hn#B00008"
        done
      done
      plusargs=()
      [ -z "$plus" ] || plusargs=(--plus "$plus")
      "$RUN" --orca "$ORCA" --map "0=$work" --sfile "$base.c" --dfile "$name" \
        --lops 1 --merr 8 --minus "$minus" "''${plusargs[@]}" \
        "$ORCA/Languages/cc#B5DB01" compile "$base.c" "keep=$name"
      mkdir -p "$outdir"
      found=0
      for f in "$work/$name.root#"* "$work/$name.a#"* "$work/$name.b#"*; do
        [ -f "$f" ] || continue
        out=$(basename "$f"); out=''${out%%#*}
        cp "$f" "$outdir/$out"; found=1
      done
      [ "$found" = 1 ] || { echo "orcacc: the compiler produced no object file" >&2; exit 1; }
    '';
  };

  orcalink = writeShellApplication {
    name = "orcalink";
    text = ''
      ${common}
      out=""; outdir="."; names=()
      while [ $# -gt 0 ]; do
        case "$1" in
          -o) out=$2; shift 2 ;;
          -d) outdir=$2; shift 2 ;;
          -*) echo "orcalink: unknown option $1" >&2; exit 2 ;;
          *) names+=("$1"); shift ;;
        esac
      done
      [ "''${#names[@]}" -gt 0 ] || { echo "usage: orcalink [-o OUT] [-d OUTDIR] NAME..." >&2; exit 2; }
      [ -n "$out" ] || out=$(basename "''${names[0]}")
      # The linker finds NAME.root, NAME.a, NAME.b ... by name in prefix 0.
      list=""
      for n in "''${names[@]}"; do
        b=$(basename "$n"); d=$(dirname "$n")
        for f in "$d/$b.root" "$d/$b".[a-z]; do
          [ -f "$f" ] || continue
          cp "$f" "$work/$(basename "$f")#B10000"
        done
        list="$list''${list:+ }$b"
      done
      # +S: the linker prints its global symbol table, (offset, segment) per
      # symbol. It does not change the load file (checked byte for byte). It is
      # kept as OUT.sym, and omf2bin --syms turns it into addresses for a2min's
      # function profiler (A2MIN_SYMBOLS); the console shows the rest as before.
      if ! "$RUN" --orca "$ORCA" --map "0=$work" --sfile "$list" --dfile "$out" \
          --lops 2 --merr 8 --plus S \
          "$ORCA/Languages/Linker#B5DB01" link "$list" "keep=$out" > "$work/link.log" 2>&1; then
        cat "$work/link.log"; exit 1
      fi
      sed '/Global symbol table:/,/^There are/{/^There are/!d}' "$work/link.log"
      mkdir -p "$outdir"
      sed -n '/Global symbol table:/,/^There are/p' "$work/link.log" > "$outdir/$out.sym"
      found=""
      for f in "$work/$out#"*; do
        [ -f "$f" ] || continue
        found=$f; break
      done
      [ -n "$found" ] || { echo "orcalink: the linker produced no load file" >&2; exit 1; }
      cp "$found" "$outdir/$out"
    '';
  };

  omf2bin = writeShellApplication {
    name = "omf2bin";
    text = ''
      exec ${orcarun}/bin/orcarun --omf2bin "$@"
    '';
  };

  orcabuild = writeShellApplication {
    name = "orcabuild";
    runtimeInputs = [ orcacc orcalink omf2bin ];
    text = ''
      out=""; opts=(); srcs=(); org=()
      while [ $# -gt 0 ]; do
        case "$1" in
          -o) out=$2; shift 2 ;;
          -I) opts+=(-I "$2"); shift 2 ;;
          -O) opts+=(-O); shift ;;
          --org) org=(--org "$2"); shift 2 ;;
          -*) echo "orcabuild: unknown option $1" >&2; exit 2 ;;
          *) srcs+=("$1"); shift ;;
        esac
      done
      [ -n "$out" ] && [ "''${#srcs[@]}" -gt 0 ] \
        || { echo "usage: orcabuild -o OUT.bin [-O] [-I DIR]... [--org N] FILE.c..." >&2; exit 2; }
      stage=$(mktemp -d)
      trap 'rm -rf "$stage"' EXIT
      names=()
      for s in "''${srcs[@]}"; do
        n=$(basename "$s"); n=''${n%.*}
        orcacc "''${opts[@]}" -d "$stage" -o "$n" "$s"
        names+=("$stage/$n")
      done
      name=$(basename "$out"); name=''${name%.*}
      orcalink -d "$stage" -o "$name" "''${names[@]}"
      # OUT.map: "ADDRESS NAME" per symbol, for A2MIN_SYMBOLS (docs/VM.md)
      omf2bin "''${org[@]}" --syms "$stage/$name.sym" "''${out%.*}.map" "$stage/$name" "$out"
      # the OMF load file next to the binary, for coff/lseg-style inspection
      cp "$stage/$name" "''${out%.*}.omf"
    '';
  };
in
symlinkJoin {
  name = "orca-toolchain";
  paths = [ orcacc orcalink omf2bin orcabuild orcarun ];
  meta = {
    description = "orcacc/orcalink/omf2bin/orcabuild: ORCA/C as host commands";
  };
}
