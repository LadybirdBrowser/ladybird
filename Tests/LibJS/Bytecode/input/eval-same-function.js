// Test that eval in the same function prevents using GetGlobal
// for potentially shadowable identifiers.
// The function should use GetBinding for Number, not GetGlobal.

function foo() {
    eval("var x = 1");
    return new Number(42);
}

foo();
