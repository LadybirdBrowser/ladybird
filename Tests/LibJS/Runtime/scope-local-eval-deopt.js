/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

describe("scope-local eval deoptimization", () => {
    test("eval in child function does not affect parent lookups", () => {
        function outer() {
            let x = 100;
            function inner() {
                eval("var y = 200");
                return y;
            }
            let innerResult = inner();
            return x + innerResult;
        }
        expect(outer()).toBe(300);
    });

    test("eval shadows variable from outer function within same function", () => {
        function outer() {
            var c = 1;
            function inner() {
                let before = c;
                eval("var c = 2");
                let after = c;
                return before * 10 + after;
            }
            return inner();
        }
        expect(outer()).toBe(12);
    });

    test("conditional eval - false", () => {
        function foo(doEval) {
            var c = 1;
            function bar(doEval) {
                if (doEval) eval("var c = 2;");
                return c;
            }
            return bar(doEval);
        }
        expect(foo(false)).toBe(1);
    });

    test("conditional eval - true", () => {
        function foo(doEval) {
            var c = 1;
            function bar(doEval) {
                if (doEval) eval("var c = 2;");
                return c;
            }
            return bar(doEval);
        }
        expect(foo(true)).toBe(2);
    });

    test("block scope within function containing eval", () => {
        function foo() {
            let outerLet = 10;
            let result = 0;
            {
                let blockLet = 20;
                eval("var evalVar = 30");
                result = outerLet + blockLet + evalVar;
            }
            return result + outerLet;
        }
        expect(foo()).toBe(70);
    });

    test("multiple levels of nesting", () => {
        function a() {
            let aVar = 1;
            function b() {
                let bVar = 2;
                function c() {
                    let cVar = 3;
                    eval("var evalVar = 4");
                    return cVar + evalVar;
                }
                return bVar + c();
            }
            return aVar + b();
        }
        expect(a()).toBe(10);
    });

    test("eval in multiple nested functions at different levels", () => {
        function a() {
            let aVar = 1;
            eval("var aEval = 10");
            function b() {
                let bVar = 2;
                eval("var bEval = 20");
                function c() {
                    let cVar = 3;
                    eval("var cEval = 30");
                    return cVar + cEval;
                }
                return bVar + bEval + c();
            }
            return aVar + aEval + b();
        }
        expect(a()).toBe(66);
    });

    test("arrow function containing eval", () => {
        function outer() {
            let x = 5;
            const arrow = () => {
                eval("var y = 10");
                return x + y;
            };
            let result = arrow();
            return result + x;
        }
        expect(outer()).toBe(20);
    });

    test("eval in for loop", () => {
        function foo() {
            let sum = 0;
            for (let i = 0; i < 3; i++) {
                eval("var loopVar = i * 10");
                sum += loopVar;
            }
            return sum;
        }
        expect(foo()).toBe(30);
    });

    test("eval creating variable that shadows outer scope", () => {
        var globalVar = 100;
        function foo() {
            let before = globalVar;
            eval("var globalVar = 200");
            let after = globalVar;
            return before * 1000 + after;
        }
        let result = foo();
        expect(result + globalVar).toBe(100300);
    });

    test("deep nesting where only innermost has eval", () => {
        function level1() {
            let v1 = 1;
            function level2() {
                let v2 = 2;
                function level3() {
                    let v3 = 3;
                    function level4() {
                        eval("var v4 = 4");
                        return v1 + v2 + v3 + v4;
                    }
                    return level4();
                }
                return level3();
            }
            return level2();
        }
        expect(level1()).toBe(10);
    });

    test("eval with same variable name at multiple levels", () => {
        function outer() {
            var x = 1;
            function middle() {
                var x = 2;
                function inner() {
                    eval("var x = 3");
                    return x;
                }
                let innerX = inner();
                return x * 10 + innerX;
            }
            let middleResult = middle();
            return x * 100 + middleResult;
        }
        expect(outer()).toBe(123);
    });

    test("strict mode eval does not affect outer scope", () => {
        function foo() {
            var x = 1;
            function bar() {
                "use strict";
                eval("var x = 2");
                return x;
            }
            let barResult = bar();
            return x * 10 + barResult;
        }
        expect(foo()).toBe(11);
    });

    test("indirect eval uses global scope", () => {
        var globalForTest = 100;
        function foo() {
            var localVar = 200;
            var globalForTest = 300;
            let directResult = eval("globalForTest");
            let indirectEval = eval;
            let indirectResult = indirectEval("typeof localVar");
            return directResult + "_" + indirectResult;
        }
        expect(foo()).toBe("300_undefined");
    });

    test("eval in catch block", () => {
        function foo() {
            var x = 1;
            try {
                throw new Error("test");
            } catch (e) {
                eval("var x = 2");
            }
            return x;
        }
        expect(foo()).toBe(2);
    });

    test("eval with function declaration", () => {
        function outer() {
            var x = 1;
            function inner() {
                eval("function f() { return 42; }");
                return f();
            }
            let result = inner();
            let fExists = typeof f !== "undefined";
            return result + "_" + fExists;
        }
        expect(outer()).toBe("42_false");
    });

    test("multiple variables created by single eval", () => {
        function foo() {
            var a = 1;
            function bar() {
                eval("var a = 10; var b = 20; var c = 30;");
                return a + b + c;
            }
            let barResult = bar();
            return a * 1000 + barResult;
        }
        expect(foo()).toBe(1060);
    });

    test("eval accessing closure variables", () => {
        function outer() {
            let closureVar = 100;
            function inner() {
                return eval("closureVar + 50");
            }
            return inner();
        }
        expect(outer()).toBe(150);
    });

    test("repeated calls to function with eval", () => {
        function outer() {
            var counter = 0;
            function inner() {
                eval("var localCounter = " + counter);
                counter++;
                return localCounter;
            }
            let r1 = inner();
            let r2 = inner();
            let r3 = inner();
            return r1 + "_" + r2 + "_" + r3 + "_" + counter;
        }
        expect(outer()).toBe("0_1_2_3");
    });

    test("eval in IIFE inside function", () => {
        function outer() {
            var x = 1;
            (function () {
                eval("var x = 2");
            })();
            return x;
        }
        expect(outer()).toBe(1);
    });

    test("eval does not affect function parameters", () => {
        function outer(a, b) {
            function inner() {
                eval("var a = 100");
                return a;
            }
            let innerResult = inner();
            return a + "_" + innerResult;
        }
        expect(outer(1, 2)).toBe("1_100");
    });

    test("eval var vs closure let", () => {
        function outer() {
            let x = 1;
            function inner() {
                let y = 2;
                eval("var x = 10");
                return x + y;
            }
            let innerResult = inner();
            return x * 100 + innerResult;
        }
        expect(outer()).toBe(112);
    });

    test("typeof on variable that might be created by eval - false", () => {
        function foo(doEval) {
            if (doEval) {
                eval("var dynamicVar = 42");
            }
            return typeof dynamicVar;
        }
        expect(foo(false)).toBe("undefined");
    });

    test("typeof on variable that might be created by eval - true", () => {
        function foo(doEval) {
            if (doEval) {
                eval("var dynamicVar = 42");
            }
            return typeof dynamicVar;
        }
        expect(foo(true)).toBe("number");
    });

    test("global-level eval creates global variable", () => {
        // Use indirect eval to execute in global scope
        const globalEval = eval;
        globalEval("var globalTestVar123 = 999");
        expect(globalThis.globalTestVar123).toBe(999);
        delete globalThis.globalTestVar123;
    });

    test("global-level eval can modify existing global", () => {
        globalThis.existingGlobal456 = 100;
        const globalEval = eval;
        globalEval("existingGlobal456 = 200");
        expect(globalThis.existingGlobal456).toBe(200);
        delete globalThis.existingGlobal456;
    });

    test("direct eval in function does not create true global", () => {
        function foo() {
            eval("var localToFoo = 42");
            return localToFoo;
        }
        expect(foo()).toBe(42);
        expect(typeof localToFoo).toBe("undefined");
    });

    test("eval shadowing global does not modify actual global", () => {
        globalThis.testGlobal789 = "original";
        function foo() {
            eval("var testGlobal789 = 'shadowed'");
            return testGlobal789;
        }
        expect(foo()).toBe("shadowed");
        expect(globalThis.testGlobal789).toBe("original");
        delete globalThis.testGlobal789;
    });

    test("nested function accesses eval-created var in parent, not global", () => {
        globalThis.sharedName111 = "global";
        function outer() {
            eval("var sharedName111 = 'local'");
            function inner() {
                return sharedName111;
            }
            return inner();
        }
        expect(outer()).toBe("local");
        expect(globalThis.sharedName111).toBe("global");
        delete globalThis.sharedName111;
    });

    test("eval as subexpression of binary operator", () => {
        function foo(n) {
            let local = n | 0;
            let code = "local + 11";
            return eval(code) | 0;
        }
        expect(foo(3)).toBe(14);
    });

    test("eval as subexpression of ternary operator", () => {
        function foo(n) {
            let local = n;
            return eval("local") ? "truthy" : "falsy";
        }
        expect(foo(1)).toBe("truthy");
        expect(foo(0)).toBe("falsy");
    });

    test("eval as argument to another function call", () => {
        function foo(n) {
            let local = n * 2;
            return String(eval("local"));
        }
        expect(foo(21)).toBe("42");
    });

    test("eval in comma expression", () => {
        function foo(n) {
            let local = n;
            return (0, eval("local + 1"));
        }
        expect(foo(5)).toBe(6);
    });

    test("eval result used in assignment expression", () => {
        function foo(n) {
            let local = n;
            let result = eval("local") + 100;
            return result;
        }
        expect(foo(23)).toBe(123);
    });
});
