# C++ Nix: functions never compare equal at the top level, but the pointer shortcut applies inside containers
let f = x: x; g = f; in [ (f == f) (f == g) ([ f ] == [ f ]) ({ inherit f; } == { f = g; }) ([ f ] == [ (x: x) ]) ]
