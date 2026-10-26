# __toString's result is coerced as "${}" does: an integer is a type error.
builtins.toJSON { __toString = s: 1; }
