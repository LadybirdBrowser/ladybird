// Test that identifiers at program scope are promoted to globals.
// The bytecode should use GetGlobal/SetGlobal, not GetBinding/SetBinding.

var x = 1;
console.log(x);
