// Test that computed member reads don't generate an unnecessary Mov
// to save the property register (only needed for store-back operations).

function computed_read(a) {
    return a[0];
}

function computed_read_expression(a, i) {
    return a[i];
}

computed_read([1]);
computed_read_expression([1], 0);
