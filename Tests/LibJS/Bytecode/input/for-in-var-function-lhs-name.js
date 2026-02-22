// Test that B.3.5 for-in head initializers with function expressions
// produce NewFunction with the correct lhs_name.
function fn() {
    for (var f = (function () {}) in {}) {
    }
}
fn();
