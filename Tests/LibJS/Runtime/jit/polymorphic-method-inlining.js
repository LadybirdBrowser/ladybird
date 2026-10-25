// JIT code inlines each method a call site may call where the callee is one
// of the methods a property lookup found for the shapes it switched on, and
// speculates on shapes again once the property lookup cache learned the
// shapes it exited for.
// These tests run every function many times, so that with a low JIT
// threshold they are compiled.

const iterations = 100;

describe("inlined methods of polymorphic receivers", () => {
    test("methods of several classes through one call site", () => {
        class Circle {
            constructor(r) {
                this.r = r;
            }
            area() {
                return 3 * this.r * this.r;
            }
        }
        class Square {
            constructor(s) {
                this.s = s;
            }
            area() {
                return this.s * this.s;
            }
        }
        class Rectangle {
            constructor(w, h) {
                this.w = w;
                this.h = h;
            }
            area() {
                return this.w * this.h;
            }
        }
        function total(shapes) {
            let sum = 0;
            for (let i = 0; i < shapes.length; ++i) sum += shapes[i].area();
            return sum;
        }
        const shapes = [new Circle(1), new Square(2), new Rectangle(2, 3), new Square(1)];
        for (let i = 0; i < iterations; ++i) expect(total(shapes)).toBe(3 + 4 + 6 + 1);
    });

    test("receivers of shapes the call site sees later", () => {
        function makeClass(k) {
            return class {
                get() {
                    return k;
                }
            };
        }
        const classes = [makeClass(1), makeClass(2), makeClass(3), makeClass(4), makeClass(5), makeClass(6)];
        function call(object) {
            return object.get();
        }
        for (let phase = 1; phase <= classes.length; ++phase) {
            const objects = classes.slice(0, phase).map(C => new C());
            for (let i = 0; i < iterations; ++i) {
                let sum = 0;
                for (const object of objects) sum += call(object);
                expect(sum).toBe((phase * (phase + 1)) / 2);
            }
        }
    });

    test("methods replaced on a prototype", () => {
        class A {
            value() {
                return 1;
            }
        }
        class B {
            value() {
                return 2;
            }
        }
        function call(object) {
            return object.value();
        }
        const objects = [new A(), new B()];
        for (let i = 0; i < iterations; ++i) expect(call(objects[i % 2])).toBe((i % 2) + 1);
        B.prototype.value = function () {
            return 20;
        };
        for (let i = 0; i < iterations; ++i) expect(call(objects[i % 2])).toBe(i % 2 ? 20 : 1);
        A.prototype.value = 5;
        expect(() => call(objects[0])).toThrow(TypeError);
        expect(call(objects[1])).toBe(20);
    });

    test("methods replaced by an inlined method while the caller runs", () => {
        class A {
            value(i) {
                if (i === 150) {
                    B.prototype.value = function () {
                        return 30;
                    };
                }
                return 1;
            }
        }
        class B {
            value() {
                return 2;
            }
        }
        function sum(objects, count) {
            let total = 0;
            for (let i = 0; i < count; ++i) total += objects[i % 2].value(i);
            return total;
        }
        const objects = [new A(), new B()];
        for (let i = 0; i < 10; ++i) expect(sum(objects, 100)).toBe(150);
        // Up to i = 150, B's calls return 2, and 30 after.
        expect(sum(objects, 300)).toBe(150 + 75 * 2 + 75 * 30);
    });

    test("methods that throw, never return, or are too big to inline", () => {
        class Thrower {
            run(value) {
                throw new Error(`thrown ${value}`);
            }
        }
        class Plain {
            run(value) {
                return value + 1;
            }
        }
        class Big {
            run(value) {
                let a = value,
                    b = 1,
                    c = 2,
                    d = 3;
                for (let i = 0; i < 3; ++i) {
                    a += b * c;
                    b += c * d;
                    c += d * a;
                    d += a * b;
                    a %= 1000;
                    b %= 1000;
                    c %= 1000;
                    d %= 1000;
                    if (a > 500) a -= 7;
                    if (b > 500) b -= 11;
                    if (c > 500) c -= 13;
                    if (d > 500) d -= 17;
                }
                return a + b + c + d;
            }
        }
        function bigRun(value) {
            return new Big().run(value);
        }
        function call(object, value) {
            try {
                return object.run(value);
            } catch (error) {
                return error.message;
            }
        }
        const objects = [new Thrower(), new Plain(), new Big()];
        for (let i = 0; i < iterations; ++i) {
            expect(call(objects[0], i)).toBe(`thrown ${i}`);
            expect(call(objects[1], i)).toBe(i + 1);
            expect(call(objects[2], i)).toBe(bigRun(i));
        }
    });

    test("this, arguments and the results of the inlined methods", () => {
        class A {
            constructor() {
                this.name = "a";
            }
            describe(x, y) {
                return `${this.name}:${x}:${y}:${arguments.length}`;
            }
        }
        class B {
            constructor() {
                this.label = "b";
            }
            describe(x) {
                return [this.label, x];
            }
        }
        function call(object, i) {
            return object.describe(i, i + 1);
        }
        const objects = [new A(), new B()];
        for (let i = 0; i < iterations; ++i) {
            expect(call(objects[0], i)).toBe(`a:${i}:${i + 1}:2`);
            expect(call(objects[1], i)).toEqual(["b", i]);
        }
    });
});

describe("inlined methods of callees copied to other registers", () => {
    test("methods called through a copy of the callee", () => {
        class A {
            m(x) {
                return typeof x === "function" ? 10 : 1;
            }
        }
        class B {
            m(x) {
                return typeof x === "function" ? 20 : 2;
            }
        }
        function call(object) {
            const m = object.m;
            const n = m;
            return m() + n() + m(m);
        }
        const objects = [new A(), new B()];
        for (let i = 0; i < iterations; ++i) expect(call(objects[i % 2])).toBe(i % 2 ? 2 + 2 + 20 : 1 + 1 + 10);
    });
});

describe("methods replaced again and again", () => {
    test("methods redefined on a prototype many times", () => {
        class A {
            value() {
                return 0;
            }
        }
        function call(object) {
            return object.value();
        }
        const object = new A();
        for (let round = 1; round <= 20; ++round) {
            Object.defineProperty(A.prototype, "value", {
                value: () => round,
                writable: true,
                configurable: true,
            });
            for (let i = 0; i < iterations; ++i) expect(call(object)).toBe(round);
        }
    });
});
