// Test that chained member access produces readable expression strings
// in Call instructions (e.g., "a[b][c].foo" not "<object>.foo").

function chained_computed_call(a, j, k) {
    return a[j][k].foo();
}

function chained_dot_call(a) {
    return a.b.c.bar();
}

try {
    chained_computed_call(0, 0, 0);
} catch {}
try {
    chained_dot_call(0);
} catch {}
