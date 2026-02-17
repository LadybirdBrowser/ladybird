// Test that base_identifier for nested member expressions with
// non-identifier computed properties only includes the parts that
// resolve to identifiers (matching C++ expression_identifier).

function assign_nested(arr, i) {
    arr[i - 1].shader = 0;
}

assign_nested([{ shader: 1 }], 1);
