/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

describe("var shadowing in default parameter expressions", () => {
    test("body var shadows name used in default -- default sees outer scope", () => {
        var shadow = "outer";
        function f(x = shadow) {
            var shadow = "inner";
            return x;
        }
        expect(f()).toBe("outer");
        expect(f("explicit")).toBe("explicit");
    });

    test("multiple defaults referencing same shadowed name", () => {
        var v = "outer";
        function f(a = v, b = v) {
            var v = "inner";
            return a + " " + b;
        }
        expect(f()).toBe("outer outer");
    });

    test("destructuring default referencing shadowed name", () => {
        var obj = { x: 42 };
        function f({ x } = obj) {
            var obj = { x: 99 };
            return x;
        }
        expect(f()).toBe(42);
    });

    test("body vars not referenced in defaults stay optimized", () => {
        function f(x = 1) {
            var y = 2;
            return x + y;
        }
        expect(f()).toBe(3);
    });

    test("param re-declared with var in body retains its value", () => {
        function f(a = 10) {
            var a;
            return a;
        }
        expect(f()).toBe(10);
    });

    test("default references earlier parameter", () => {
        function f(a = 1, b = a) {
            var a = 99;
            return b;
        }
        expect(f()).toBe(1);
    });

    test("body function declaration accessible with defaults", () => {
        function f(x = 1) {
            var y = 42;
            function inner() {
                return y;
            }
            return inner();
        }
        expect(f()).toBe(42);
    });

    test("nested function scope with shadowed default", () => {
        var v = "outer";
        function outer() {
            function inner(x = v) {
                var v = "inner";
                return x;
            }
            return inner();
        }
        expect(outer()).toBe("outer");
    });

    test("helper function call in default with shadowed var", () => {
        var arr = [1, 2, 3];
        function helper() {
            return arr;
        }
        function f(x = helper()) {
            var arr = [4, 5, 6];
            return x[0];
        }
        expect(f()).toBe(1);
    });
});
