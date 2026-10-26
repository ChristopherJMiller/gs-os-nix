# appendContext: an output name may not itself carry a context (forceStringNoCtx).
let b = derivation { name = "b"; system = "s"; builder = "/bin/sh"; }; in builtins.appendContext "x" { "/nix/store/zk00i9dmrkzkyw50mcxmq9v75lq04br0-a.drv" = { outputs = [ "out${builtins.substring 0 0 "${b}"}" ]; }; }
