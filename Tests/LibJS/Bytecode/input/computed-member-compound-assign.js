// Test that compound assignment to computed member expressions
// produces optimal register allocation (reusing the property register
// after saving it), and that register free order is consistent across
// sequential statements.

function compound_computed(obj, key) {
    obj[key] += 1;
}

function compound_then_assign(imag, i) {
    imag[i] += 0;
    imag[i] = i;
}

compound_computed({}, "x");
compound_then_assign({ x: 1 }, "x");
