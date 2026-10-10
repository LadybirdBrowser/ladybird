// JIT code creates the function objects of closures itself, by copying one it
// made for the same code before. These tests run every function many times,
// so that with a low JIT threshold they are compiled.

const iterations = 100;

describe("closures created in JIT code", () => {
    test("environments, names, lengths and prototypes", () => {
        function makeAdder(x) {
            return function (y) {
                return x + y;
            };
        }
        function makeArrow(x) {
            const multiply = (y, z) => x * y;
            return multiply;
        }
        function makeNamed(i) {
            function inner(a, b, c) {
                return i;
            }
            return inner;
        }
        for (let i = 0; i < iterations; ++i) {
            const adder = makeAdder(i);
            expect(adder(1)).toBe(i + 1);
            expect(adder.name).toBe("");
            expect(adder.length).toBe(1);
            expect(typeof adder.prototype).toBe("object");
            expect(adder.prototype.constructor).toBe(adder);
            expect(adder.prototype).not.toBe(makeAdder(i).prototype);
            expect(Object.getPrototypeOf(adder)).toBe(Function.prototype);

            const arrow = makeArrow(i);
            expect(arrow(2)).toBe(2 * i);
            expect(arrow.name).toBe("multiply");
            expect(arrow.length).toBe(2);
            expect(arrow.prototype).toBeUndefined();

            const named = makeNamed(i);
            expect(named()).toBe(i);
            expect(named.name).toBe("inner");
            expect(named.length).toBe(3);
            expect(named.toString()).toBe("function inner(a, b, c) {\n                return i;\n            }");
            named.extra = i;
            expect(named.extra).toBe(i);
            expect(new named()).toBeInstanceOf(named);
        }
    });

    test("closures over mutable bindings, and collections", () => {
        function counter() {
            let count = 0;
            const increment = () => ++count;
            const read = function () {
                return count;
            };
            gc();
            return [increment, read];
        }
        function curry(fun) {
            return function (a) {
                return function (b) {
                    return fun(a, b);
                };
            };
        }
        for (let i = 0; i < iterations; ++i) {
            const [increment, read] = counter();
            increment();
            increment();
            expect(read()).toBe(2);
            expect(curry((a, b) => a - b)(i)(1)).toBe(i - 1);
        }
    });

    test("methods with home objects", () => {
        const base = {
            greet() {
                return "base";
            },
        };
        function make(i) {
            const object = {
                greet() {
                    return super.greet() + i;
                },
            };
            Object.setPrototypeOf(object, base);
            return object;
        }
        for (let i = 0; i < iterations; ++i) expect(make(i).greet()).toBe("base" + i);
    });
});
