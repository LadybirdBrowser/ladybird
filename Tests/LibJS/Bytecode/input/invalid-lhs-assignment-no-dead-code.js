// Test that assigning to an invalid LHS (like a call expression)
// does NOT emit dead code after the Throw instruction.

function f(x) {
    x() = "foo";
}

try { f(() => {}); } catch {}
