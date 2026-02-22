// Tests that lexical environment teardown works correctly for
// all code paths: with, block scope, for-in/of, catch, and
// named function expressions.

// 1. with statement restores outer environment
function withTeardown() {
    let outer = 1;
    with ({ x: 10 }) {
        let inner = x + outer;
    }
    return outer;
}
console.log(withTeardown());

// 2. Block scope with let restores environment
function blockTeardown() {
    let outer = 2;
    {
        let inner = 3;
        outer;
    }
    return outer;
}
console.log(blockTeardown());

// 3. for-in with lexical binding restores environment
function forInTeardown() {
    let outer = 4;
    for (let k in { a: 1, b: 2 }) {
        k;
    }
    return outer;
}
console.log(forInTeardown());

// 4. for-of with lexical binding restores environment
function forOfTeardown() {
    let outer = 5;
    for (let v of [10, 20]) {
        v;
    }
    return outer;
}
console.log(forOfTeardown());

// 5. try-catch restores environment after catch scope
function catchTeardown() {
    let outer = 6;
    try {
        throw new Error("test");
    } catch (e) {
        e;
    }
    return outer;
}
console.log(catchTeardown());

// 6. Named function expression scope teardown
var namedFn = function myName() {
    return typeof myName;
};
console.log(namedFn());
