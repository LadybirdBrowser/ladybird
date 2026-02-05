// Test that eval in a nested function doesn't prevent the parent function
// from using GetGlobal for global identifiers.
// The parent function should use GetGlobal for Number, not GetBinding.

function outer() {
    function inner() {
        eval("var x = 1");
    }
    return new Number(42);
}

outer();
