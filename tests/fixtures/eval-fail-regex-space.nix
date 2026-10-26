# Past the NFA's limit: Nix's "memory limit exceeded", the VM's E_NOMEM.
builtins.match "a{100001}" "a"
