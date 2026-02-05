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

// Tests for nested function patterns (typescript.js scenario)
test("nested function sees eval-shadowed undefined from parent scope", () => {
    function parent() {
        function child() {
            return undefined;
        }
        eval("var undefined = 42");
        return child();
    }
    expect(parent()).toBe(42);
});

test("nested function sees eval-shadowed NaN from parent scope", () => {
    function parent() {
        function child() {
            return NaN;
        }
        eval("var NaN = 'shadowed'");
        return child();
    }
    expect(parent()).toBe("shadowed");
});

test("nested function sees eval-shadowed Infinity from parent scope", () => {
    function parent() {
        function child() {
            return Infinity;
        }
        eval("var Infinity = 999");
        return child();
    }
    expect(parent()).toBe(999);
});

test("deeply nested function sees eval-shadowed value", () => {
    function level1() {
        function level2() {
            function level3() {
                return undefined;
            }
            return level3();
        }
        eval("var undefined = 'deep'");
        return level2();
    }
    expect(level1()).toBe("deep");
});

test("sibling nested functions both see eval-shadowed value", () => {
    function parent() {
        function child1() {
            return undefined;
        }
        function child2() {
            return undefined;
        }
        eval("var undefined = 'both'");
        return [child1(), child2()];
    }
    const result = parent();
    expect(result[0]).toBe("both");
    expect(result[1]).toBe("both");
});

test("eval in nested function shadows for that function and its children only", () => {
    function outer() {
        var outerResult = undefined;
        function middle() {
            function inner() {
                return undefined;
            }
            eval("var undefined = 'middle'");
            return inner();
        }
        return [outerResult, middle()];
    }
    const result = outer();
    expect(result[0]).toBeUndefined();
    expect(result[1]).toBe("middle");
});

// IIFE patterns
test("IIFE with eval does not affect outer scope undefined", () => {
    var outerUndefined = undefined;
    (function () {
        eval("var undefined = 'iife'");
    })();
    expect(outerUndefined).toBeUndefined();
});

test("nested function in IIFE sees eval-shadowed value", () => {
    var result = (function () {
        function inner() {
            return undefined;
        }
        eval("var undefined = 'iife-inner'");
        return inner();
    })();
    expect(result).toBe("iife-inner");
});

test("IIFE without eval does not affect undefined optimization", () => {
    var result = (function () {
        return undefined;
    })();
    expect(result).toBeUndefined();
});

test("multiple IIFEs with different eval patterns", () => {
    var result1 = (function () {
        return undefined;
    })();

    var result2 = (function () {
        eval("var undefined = 'second'");
        return undefined;
    })();

    var result3 = (function () {
        return undefined;
    })();

    expect(result1).toBeUndefined();
    expect(result2).toBe("second");
    expect(result3).toBeUndefined();
});

// Strict mode tests
test("strict mode eval does not shadow undefined in same function", () => {
    function test() {
        "use strict";
        eval("var undefined = 42");
        return undefined;
    }
    expect(test()).toBeUndefined();
});

test("strict mode eval does not shadow undefined in nested function", () => {
    function parent() {
        "use strict";
        function child() {
            return undefined;
        }
        eval("var undefined = 42");
        return child();
    }
    expect(parent()).toBeUndefined();
});

// Multiple identifiers shadowed
test("eval shadows multiple global identifiers at once", () => {
    function test() {
        eval("var undefined = 1; var NaN = 2; var Infinity = 3");
        return [undefined, NaN, Infinity];
    }
    const result = test();
    expect(result[0]).toBe(1);
    expect(result[1]).toBe(2);
    expect(result[2]).toBe(3);
});

test("nested function sees multiple shadowed identifiers", () => {
    function parent() {
        function child() {
            return [undefined, NaN, Infinity];
        }
        eval("var undefined = 'u'; var NaN = 'n'; var Infinity = 'i'");
        return child();
    }
    const result = parent();
    expect(result[0]).toBe("u");
    expect(result[1]).toBe("n");
    expect(result[2]).toBe("i");
});

// Function expression patterns
test("function expression with eval shadows for nested functions", () => {
    var outer = function () {
        var inner = function () {
            return undefined;
        };
        eval("var undefined = 'expr'");
        return inner();
    };
    expect(outer()).toBe("expr");
});

test("arrow function in regular function with eval", () => {
    function parent() {
        const arrow = () => undefined;
        eval("var undefined = 'arrow-parent'");
        return arrow();
    }
    expect(parent()).toBe("arrow-parent");
});

test("nested arrow functions with eval in outer", () => {
    function outer() {
        const level1 = () => {
            const level2 = () => undefined;
            return level2();
        };
        eval("var undefined = 'nested-arrows'");
        return level1();
    }
    expect(outer()).toBe("nested-arrows");
});

// Method and property patterns
test("method using undefined with eval in sibling method", () => {
    const obj = {
        foo() {
            return undefined;
        },
        bar() {
            eval("");
            return 42;
        },
    };
    expect(obj.foo()).toBeUndefined();
    expect(obj.bar()).toBe(42);
});

test("getter using undefined not affected by eval in setter", () => {
    const obj = {
        get value() {
            return undefined;
        },
        set value(v) {
            eval("");
        },
    };
    expect(obj.value).toBeUndefined();
    obj.value = 1;
    expect(obj.value).toBeUndefined();
});

// Callback and closure patterns
test("callback sees eval-shadowed value from enclosing scope", () => {
    function withCallback(cb) {
        return cb();
    }

    function outer() {
        eval("var undefined = 'callback'");
        return withCallback(function () {
            return undefined;
        });
    }
    expect(outer()).toBe("callback");
});

test("closure captures eval-shadowed value", () => {
    function createClosure() {
        eval("var undefined = 'closure'");
        return function () {
            return undefined;
        };
    }
    const fn = createClosure();
    expect(fn()).toBe("closure");
});

test("multiple closures from same scope with eval", () => {
    function createClosures() {
        eval("var undefined = 'multi'");
        return [
            function () {
                return undefined;
            },
            function () {
                return undefined;
            },
            function () {
                return undefined;
            },
        ];
    }
    const fns = createClosures();
    expect(fns[0]()).toBe("multi");
    expect(fns[1]()).toBe("multi");
    expect(fns[2]()).toBe("multi");
});

// Try-catch-finally patterns
test("eval in try block shadows for nested function", () => {
    function test() {
        function inner() {
            return undefined;
        }
        try {
            eval("var undefined = 'try'");
        } catch (e) {}
        return inner();
    }
    expect(test()).toBe("try");
});

test("eval in catch block shadows for nested function", () => {
    function test() {
        function inner() {
            return undefined;
        }
        try {
            throw new Error();
        } catch (e) {
            eval("var undefined = 'catch'");
        }
        return inner();
    }
    expect(test()).toBe("catch");
});

test("eval in finally block shadows for nested function", () => {
    function test() {
        function inner() {
            return undefined;
        }
        try {
        } finally {
            eval("var undefined = 'finally'");
        }
        return inner();
    }
    expect(test()).toBe("finally");
});

// Loop patterns
test("eval in loop body shadows for nested function", () => {
    function test() {
        function inner() {
            return undefined;
        }
        for (var i = 0; i < 1; i++) {
            eval("var undefined = 'loop'");
        }
        return inner();
    }
    expect(test()).toBe("loop");
});

test("function defined in loop not affected by eval in sibling iteration", () => {
    var fns = [];
    for (var i = 0; i < 3; i++) {
        if (i === 1) {
            eval("");
        }
        fns.push(
            (function (x) {
                return function () {
                    return undefined;
                };
            })(i)
        );
    }
    expect(fns[0]()).toBeUndefined();
    expect(fns[1]()).toBeUndefined();
    expect(fns[2]()).toBeUndefined();
});

// Conditional patterns
test("eval in if branch shadows for nested function", () => {
    function test(condition) {
        function inner() {
            return undefined;
        }
        if (condition) {
            eval("var undefined = 'if-true'");
        }
        return inner();
    }
    expect(test(true)).toBe("if-true");
    expect(test(false)).toBeUndefined();
});

test("eval in else branch shadows for nested function", () => {
    function test(condition) {
        function inner() {
            return undefined;
        }
        if (condition) {
        } else {
            eval("var undefined = 'else'");
        }
        return inner();
    }
    expect(test(true)).toBeUndefined();
    expect(test(false)).toBe("else");
});

// Switch patterns
test("eval in switch case shadows for nested function", () => {
    function test(val) {
        function inner() {
            return undefined;
        }
        switch (val) {
            case 1:
                eval("var undefined = 'case1'");
                break;
            case 2:
                break;
        }
        return inner();
    }
    expect(test(1)).toBe("case1");
    expect(test(2)).toBeUndefined();
});

// Default parameter patterns
test("default parameter using undefined not affected by eval in function body", () => {
    function test(x = undefined) {
        eval("");
        return x;
    }
    expect(test()).toBeUndefined();
});

// Rest and spread patterns
test("rest parameter function not affected by eval in sibling", () => {
    function withRest(...args) {
        return undefined;
    }
    function withEval() {
        eval("");
    }
    expect(withRest(1, 2, 3)).toBeUndefined();
    withEval();
});

// Computed property patterns
test("computed property using undefined not affected by eval in method", () => {
    const key = undefined;
    const obj = {
        [key]: "value",
        method() {
            eval("");
        },
    };
    expect(obj[undefined]).toBe("value");
});

// Template literal patterns
test("template literal using undefined not affected by eval in sibling", () => {
    function noEval() {
        return `value is ${undefined}`;
    }
    function withEval() {
        eval("");
    }
    expect(noEval()).toBe("value is undefined");
    withEval();
});

// Destructuring patterns
test("destructuring default using undefined not affected by eval in sibling", () => {
    function noEval() {
        const { a = undefined } = {};
        return a;
    }
    function withEval() {
        eval("");
    }
    expect(noEval()).toBeUndefined();
    withEval();
});

// Generator patterns
test("generator using undefined not affected by eval in sibling", () => {
    function* gen() {
        yield undefined;
        yield undefined;
    }
    function withEval() {
        eval("");
    }
    const g = gen();
    expect(g.next().value).toBeUndefined();
    withEval();
    expect(g.next().value).toBeUndefined();
});

// Async patterns
test("async function using undefined not affected by eval in sibling", async () => {
    async function noEval() {
        return undefined;
    }
    function withEval() {
        eval("");
    }
    withEval();
    expect(await noEval()).toBeUndefined();
});

// Class patterns
test("class method using undefined not affected by eval in sibling method", () => {
    class Test {
        foo() {
            return undefined;
        }
        bar() {
            eval("");
            return 42;
        }
    }
    const t = new Test();
    expect(t.foo()).toBeUndefined();
    expect(t.bar()).toBe(42);
});

test("class constructor using undefined not affected by eval in method", () => {
    class Test {
        constructor() {
            this.value = undefined;
        }
        method() {
            eval("");
        }
    }
    const t = new Test();
    expect(t.value).toBeUndefined();
    t.method();
});

test("static method using undefined not affected by eval in instance method", () => {
    class Test {
        static foo() {
            return undefined;
        }
        bar() {
            eval("");
        }
    }
    expect(Test.foo()).toBeUndefined();
    new Test().bar();
});
