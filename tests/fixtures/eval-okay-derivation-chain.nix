# Five derivations, each an input of the next: derivationStrict nests a level
# of C stack for each (~1 KB), so this is the deepest chain the VM's stack
# holds with its 1 KB margin (VM.md "The C stack"), and scripts/stack-stress.sh
# runs it at every stack depth. A sixth level is E_NOMEM; the margin's
# predecessor let a seventh write over the harness's flags.
let
  step = prev: i: derivation {
    name = "step-${toString i}";
    system = "w65-gsos";
    builder = "/bin/sh";
    args = [ "-c" "echo ${prev} > $out" ];
  };
  chain = builtins.foldl' step "start" (builtins.genList (i: i) 5);
in chain.drvPath
