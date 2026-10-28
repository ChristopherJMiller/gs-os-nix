# Sixteen derivations, each an input of the next: derivationStrict nests a
# level of C stack for each (~1.1 KB), so this is one short of the deepest
# chain the VM's stack holds (17; VM.md "The C stack"), and
# scripts/stack-stress.sh runs it at every stack depth. When the stack was
# under the program (to 2026-10-27, D31) a sixth level was E_NOMEM, and the
# 1 KB margin's predecessor let a seventh write over the harness's flags.
let
  step = prev: i: derivation {
    name = "step-${toString i}";
    system = "w65-gsos";
    builder = "/bin/sh";
    args = [ "-c" "echo ${prev} > $out" ];
  };
  chain = builtins.foldl' step "start" (builtins.genList (i: i) 16);
in chain.drvPath
