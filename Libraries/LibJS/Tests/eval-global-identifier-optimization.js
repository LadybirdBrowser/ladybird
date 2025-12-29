test("undefined is not affected by eval in sibling function", () => {
    function foo() {
        return undefined;
    }

    function bar() {
        eval("");
        return 42;
    }

    expect(foo()).toBeUndefined();
    expect(bar()).toBe(42);
});

test("undefined works in function without eval", () => {
    function test() {
        return undefined;
    }
    expect(test()).toBeUndefined();
});

test("undefined works in function with eval that does not shadow it", () => {
    function test() {
        var result = undefined;
        eval("");
        return result;
    }
    expect(test()).toBeUndefined();
});

test("eval in function prevents optimization of identifiers in that function", () => {
    // The presence of eval() in a function prevents compile-time optimization
    // of global identifiers in that function, allowing eval to shadow them at runtime.
    function test() {
        eval("var undefined = 42");
        return undefined;
    }
    expect(test()).toBe(42);
});

test("NaN is not affected by eval in sibling function", () => {
    function foo() {
        return NaN;
    }

    function bar() {
        eval("");
        return 42;
    }

    expect(foo()).toBeNaN();
    expect(bar()).toBe(42);
});

test("Infinity is not affected by eval in sibling function", () => {
    function foo() {
        return Infinity;
    }

    function bar() {
        eval("");
        return 42;
    }

    expect(foo()).toBe(Infinity);
    expect(bar()).toBe(42);
});

test("multiple global identifiers with eval in one function", () => {
    function withoutEval() {
        return [undefined, NaN, Infinity];
    }

    function withEval() {
        eval("");
        return 42;
    }

    const result = withoutEval();
    expect(result[0]).toBeUndefined();
    expect(result[1]).toBeNaN();
    expect(result[2]).toBe(Infinity);
    expect(withEval()).toBe(42);
});

test("eval in nested function does not affect outer function globals", () => {
    function outer() {
        var u = undefined;
        function inner() {
            eval("");
        }
        inner();
        return u;
    }
    expect(outer()).toBeUndefined();
});

test("undefined optimization with eval before reference", () => {
    function test() {
        eval("");
        return undefined;
    }
    expect(test()).toBeUndefined();
});

test("undefined optimization with eval after reference", () => {
    function test() {
        var result = undefined;
        eval("");
        return result;
    }
    expect(test()).toBeUndefined();
});

test("undefined in arrow function without eval", () => {
    const test = () => undefined;
    expect(test()).toBeUndefined();
});

test("undefined in arrow function with eval in sibling", () => {
    const foo = () => undefined;
    const bar = () => {
        eval("");
        return 42;
    };

    expect(foo()).toBeUndefined();
    expect(bar()).toBe(42);
});

test("global identifiers in multiple nested scopes with eval", () => {
    function outer() {
        function middle() {
            function inner() {
                return undefined;
            }
            return inner();
        }
        return middle();
    }

    function withEval() {
        eval("");
    }

    expect(outer()).toBeUndefined();
    withEval();
});

test("eval can create variables that shadow global identifiers", () => {
    function testVarShadow() {
        eval("var undefined = 'shadowed'");
        return undefined;
    }

    function testNoEval() {
        // No eval means optimization works
        return undefined;
    }

    expect(testVarShadow()).toBe("shadowed");
    expect(testNoEval()).toBeUndefined();
});

test("undefined after eval that modifies local scope", () => {
    function test() {
        var x = 10;
        eval("x = 20");
        return [x, undefined];
    }
    const result = test();
    expect(result[0]).toBe(20);
    expect(result[1]).toBeUndefined();
});

test("multiple functions with different eval patterns", () => {
    function noEval1() {
        return undefined;
    }

    function hasEval1() {
        eval("var x = 1");
        return undefined;
    }

    function noEval2() {
        return NaN;
    }

    function hasEval2() {
        eval("");
        return Infinity;
    }

    expect(noEval1()).toBeUndefined();
    expect(hasEval1()).toBeUndefined();
    expect(noEval2()).toBeNaN();
    expect(hasEval2()).toBe(Infinity);
});

test("eval does not affect global identifier in outer scope", () => {
    var result;

    function outer() {
        result = undefined;

        function inner() {
            eval("var y = 42");
        }

        inner();
    }

    outer();
    expect(result).toBeUndefined();
});

test("complex nesting with eval only in innermost function", () => {
    function level1() {
        return (function level2() {
            return (function level3() {
                return (function level4() {
                    eval("");
                    return undefined;
                })();
            })();
        })();
    }

    expect(level1()).toBeUndefined();
});

test("eval in one branch does not affect other branches", () => {
    function branch1() {
        return undefined;
    }

    function branch2() {
        eval("");
        return NaN;
    }

    function branch3() {
        return Infinity;
    }

    expect(branch1()).toBeUndefined();
    expect(branch2()).toBeNaN();
    expect(branch3()).toBe(Infinity);
});
