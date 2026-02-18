// A body `var` that shadows a name referenced in a default parameter
// expression must NOT be optimized to a local -- the default expression
// must resolve the name from the outer scope via the environment chain.
var shadow = "outer";
function shadow_in_default(x = shadow) {
    var shadow = "inner";
    return x;
}

// Body vars whose names are NOT referenced in any default parameter
// expression should still be optimized to locals.
function no_conflict(x = 1) {
    var y = 2;
    return y;
}

// A parameter that is also var-declared in the body (var a) keeps its
// argument slot -- the IsForbiddenLexical flag prevents the deopt.
function param_redeclared(a = 10) {
    var a;
    return a;
}

// Multiple defaults with shadowing -- both body vars should be deoptimized.
function multi_shadow(x = shadow, y = shadow) {
    var shadow = "inner";
    return x;
}

// A var that shadows something referenced in a *destructuring* default
// should also be deoptimized.
function destruct_shadow({ x } = shadow) {
    var shadow = {};
    return x;
}

// An arrow function inside a default expression captures through the
// environment; `shadow` in the arrow is in its own scope and propagates
// up as captured_by_nested_function, which already prevents locals.
function arrow_in_default(x = (() => shadow)()) {
    var shadow = "inner";
    return x;
}

// Body function declarations whose names are NOT referenced in any
// default parameter expression should remain locals.
function func_decl_no_conflict(x = 1) {
    var y = 42;
    function inner() { return y; }
    return inner();
}
