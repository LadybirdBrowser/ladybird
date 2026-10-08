// Test that a named function expression only gets an environment for its name
// when something can look the name up: the function itself, a nested function
// or a direct eval in either.

function outer(x) {
    const unused = function f() {
        return x;
    };
    const used = function g() {
        return g;
    };
    const nested = function h() {
        return () => h;
    };
    return [unused, used, nested];
}

function outerWithEval() {
    return function i() {
        return () => eval("1");
    };
}

outer(1);
outerWithEval();
