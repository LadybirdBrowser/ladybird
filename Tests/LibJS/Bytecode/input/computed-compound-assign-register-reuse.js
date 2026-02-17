// Test that register free order after computed compound assignment
// matches C++ so that sequential statements reuse the same registers.

function subVector(self, v) {
    self[0] -= v[0];
    self[1] -= v[1];
}

subVector([1, 2], [3, 4]);
