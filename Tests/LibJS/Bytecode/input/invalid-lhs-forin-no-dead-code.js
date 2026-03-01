// Test that for-in with an invalid LHS (like a call expression)
// does NOT emit dead code after the Throw instruction.

function f() {}

for (f() in []);
