// Test that var-declared identifiers use GetInitializedBinding (no TDZ check)
// while let/const-declared identifiers use GetBinding (with TDZ check).

function var_access() {
    var x = 1;
    // Capture x to prevent local optimization.
    (function () {
        x;
    });
    return x;
}

function let_access() {
    let x = 1;
    // Capture x to prevent local optimization.
    (function () {
        x;
    });
    return x;
}

var_access();
let_access();
