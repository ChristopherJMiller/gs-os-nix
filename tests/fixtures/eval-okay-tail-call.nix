# tail calls must not grow the CPU stack: 2000 deep would overflow the 2 KB frame area otherwise
let f = n: if n == 0 then 0 else f (n - 1); in f 400
