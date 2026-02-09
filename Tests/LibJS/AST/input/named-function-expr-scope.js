// Named function expression: the name is local to the function body,
// not visible outside.
var f = function myFunc() {
    return myFunc;
};

// The name binding is immutable and doesn't leak.
var g = function gName(x) {
    return gName(x - 1);
};

// Named function expression inside another function.
function outer() {
    var inner = function innerName() {
        return innerName;
    };
    return inner;
}

// Recursive named function expression with closure.
function make_counter() {
    let count = 0;
    return function tick() {
        count++;
        return tick;
    };
}
