let x = 1; in let inherit x; y = { inherit x; z = "\n\"\\ \${a}"; }; in y
