// Calls made from JIT code take fast paths for ECMAScript functions, raw
// native functions, bound functions and Function.prototype.call/apply, and
// constructing ECMAScript functions and classes takes one as well. These
// tests run every caller and callee many times, so that with a low JIT
// threshold they are compiled.

const iterations = 50;

describe("calls from JIT code", () => {
    test("direct calls with missing and extra arguments", () => {
        function callee(a, b, c) {
            return [a, b, c, arguments.length];
        }
        function caller(i) {
            return [callee(i), callee(i, 1, 2, 3)];
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(i)).toEqual([
                [i, undefined, undefined, 1],
                [i, 1, 2, 4],
            ]);
        }
    });

    test("this values of sloppy and strict callees", () => {
        function sloppy() {
            return this;
        }
        function strict() {
            "use strict";
            return this;
        }
        function caller() {
            return [sloppy(), strict(), sloppy.call(1), strict.call(1)];
        }
        for (let i = 0; i < iterations; ++i) {
            const [sloppyThis, strictThis, boxedThis, primitiveThis] = caller();
            expect(sloppyThis).toBe(globalThis);
            expect(strictThis).toBeUndefined();
            expect(boxedThis).toBeInstanceOf(Number);
            expect(primitiveThis).toBe(1);
        }
    });

    test("exceptions reach the caller's handler", () => {
        function thrower(value) {
            if (value % 2) throw new Error(`odd ${value}`);
            return value;
        }
        function middle(value) {
            return thrower(value) + 1;
        }
        function caller(value) {
            try {
                return middle(value);
            } catch (error) {
                return error.message;
            }
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(i % 2 ? `odd ${i}` : i + 1);
    });

    test("exceptions from native functions reach the caller's handler", () => {
        function caller(value) {
            try {
                return JSON.parse(value);
            } catch (error) {
                return error.name;
            }
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i % 2 ? "{" : `${i}`)).toBe(i % 2 ? "SyntaxError" : i);
    });

    test("stack traces include callers", () => {
        function inner() {
            return new Error().stack;
        }
        function outer() {
            return inner();
        }
        for (let i = 0; i < iterations; ++i) {
            const stack = outer();
            expect(stack.includes("at inner")).toBeTrue();
            expect(stack.includes("at outer")).toBeTrue();
        }
    });

    test("Function.prototype.call", () => {
        function add(a, b) {
            return this.base + a + b;
        }
        function caller(i) {
            return add.call({ base: i }, 1, 2);
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(i + 3);
    });

    test("Function.prototype.call of a non-callable value throws", () => {
        const call = Function.prototype.call;
        function caller() {
            return call.call({});
        }
        for (let i = 0; i < iterations; ++i) expect(caller).toThrow(TypeError);
    });

    test("Function.prototype.apply with arrays, arguments objects, array-likes and nullish values", () => {
        function collect() {
            return Array.from(arguments);
        }
        function forward() {
            return collect.apply(null, arguments);
        }
        function caller(i) {
            return [
                collect.apply(null, [i, 1]),
                forward(i, 2),
                collect.apply(null, { length: 2, 0: i, 1: 3 }),
                collect.apply(null, null),
                collect.apply(null),
                collect.apply(null, [i, , 4]),
            ];
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(i)).toEqual([[i, 1], [i, 2], [i, 3], [], [], [i, undefined, 4]]);
        }
    });

    test("Function.prototype.apply reads array-likes in order and propagates their exceptions", () => {
        function collect() {
            return Array.from(arguments);
        }
        function caller(throwAt) {
            const reads = [];
            const arrayLike = {
                get length() {
                    reads.push("length");
                    return 2;
                },
                get 0() {
                    reads.push(0);
                    if (throwAt === 0) throw new Error("no");
                    return "a";
                },
                get 1() {
                    reads.push(1);
                    return "b";
                },
            };
            try {
                return [collect.apply(null, arrayLike), reads];
            } catch (error) {
                return [error.message, reads];
            }
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(-1)).toEqual([
                ["a", "b"],
                ["length", 0, 1],
            ]);
            expect(caller(0)).toEqual(["no", ["length", 0]]);
        }
    });

    test("Function.prototype.apply with a non-object argument list throws", () => {
        function f() {}
        function caller() {
            return f.apply(null, 1);
        }
        for (let i = 0; i < iterations; ++i) expect(caller).toThrow(TypeError);
    });

    test("bound functions", () => {
        function add(a, b, c) {
            return this.base + a + b + (c ?? 0);
        }
        const bound = add.bind({ base: 10 });
        const boundWithArguments = add.bind({ base: 20 }, 1);
        const doublyBound = boundWithArguments.bind(null, 2);
        function caller(i) {
            return [
                bound(i, 1),
                boundWithArguments(i),
                doublyBound(i),
                bound.call(null, i, 2),
                bound.apply(null, [i, 3]),
            ];
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toEqual([11 + i, 21 + i, 23 + i, 12 + i, 13 + i]);
    });

    test("class constructors cannot be called", () => {
        class C {}
        function caller() {
            return C();
        }
        for (let i = 0; i < iterations; ++i) expect(caller).toThrow(TypeError);
    });

    test("proxies and native functions", () => {
        const proxy = new Proxy(function (a) {
            return a * 2;
        }, {});
        function caller(i) {
            return [proxy(i), Math.max(i, 3), String.fromCharCode(65 + (i % 26))];
        }
        for (let i = 0; i < iterations; ++i)
            expect(caller(i)).toEqual([i * 2, Math.max(i, 3), String.fromCharCode(65 + (i % 26))]);
    });

    test("constructing ECMAScript functions", () => {
        function Point(x) {
            this.x = x;
        }
        function ReturnsObject() {
            this.ignored = true;
            return { replaced: true };
        }
        function ReturnsPrimitive() {
            this.kept = true;
            return 1;
        }
        function WithoutObjectPrototype() {}
        WithoutObjectPrototype.prototype = 1;
        function NewTarget() {
            this.target = new.target;
        }
        function caller(i) {
            return [
                new Point(i),
                new ReturnsObject(),
                new ReturnsPrimitive(),
                new WithoutObjectPrototype(),
                new NewTarget(),
            ];
        }
        for (let i = 0; i < iterations; ++i) {
            const [point, returnsObject, returnsPrimitive, withoutObjectPrototype, newTarget] = caller(i);
            expect(point).toBeInstanceOf(Point);
            expect(point.x).toBe(i);
            expect(returnsObject).toEqual({ replaced: true });
            expect(returnsPrimitive).toBeInstanceOf(ReturnsPrimitive);
            expect(returnsPrimitive.kept).toBeTrue();
            expect(Object.getPrototypeOf(withoutObjectPrototype)).toBe(Object.prototype);
            expect(newTarget.target).toBe(NewTarget);
        }
    });

    test("constructing classes", () => {
        let initialized = 0;
        class Base {
            field = ++initialized;
            constructor(x) {
                this.x = x;
            }
        }
        class Derived extends Base {
            constructor(x) {
                super(x);
                this.y = x + 1;
            }
        }
        class DerivedReturningObject extends Base {
            constructor() {
                super(0);
                return { replaced: true };
            }
        }
        function caller(i) {
            return [new Base(i), new Derived(i), new DerivedReturningObject()];
        }
        for (let i = 0; i < iterations; ++i) {
            const [base, derived, derivedReturningObject] = caller(i);
            expect(base).toBeInstanceOf(Base);
            expect(base.x).toBe(i);
            expect(derived).toBeInstanceOf(Derived);
            expect(derived.x).toBe(i);
            expect(derived.y).toBe(i + 1);
            expect(derived.field).toBe(base.field + 1);
            expect(derivedReturningObject).toEqual({ replaced: true });
        }
        expect(initialized).toBe(3 * iterations);
    });

    test("constructors that throw or return invalid values", () => {
        class ThrowingField {
            field = (() => {
                throw new Error("field");
            })();
        }
        class Base {}
        class DerivedReturningPrimitive extends Base {
            constructor() {
                super();
                return 1;
            }
        }
        class DerivedWithoutSuper extends Base {
            constructor() {}
        }
        function Throwing() {
            throw new Error("body");
        }
        function construct(Constructor) {
            try {
                new Constructor();
                return "no exception";
            } catch (error) {
                return `${error.name}: ${error.message.includes("field") || error.message.includes("body") ? error.message : ""}`;
            }
        }
        for (let i = 0; i < iterations; ++i) {
            expect(construct(ThrowingField)).toBe("Error: field");
            expect(construct(Throwing)).toBe("Error: body");
            expect(construct(DerivedReturningPrimitive)).toBe("TypeError: ");
            expect(construct(DerivedWithoutSuper)).toBe("ReferenceError: ");
        }
    });

    test("unbounded recursion throws", () => {
        function recurse(depth) {
            return recurse(depth + 1) + 1;
        }
        function recurseThroughCall(depth) {
            return recurseThroughCall.call(null, depth + 1) + 1;
        }
        expect(() => recurse(0)).toThrow(InternalError);
        expect(() => recurseThroughCall(0)).toThrow(InternalError);
    });
});
