// JIT code inlines functions that use their arguments object, which they
// never create, and calls of methods it knows from the shape of an object it
// created. These tests run every function many times, so that with a low JIT
// threshold they are compiled.

const iterations = 100;

describe("arguments objects of inlined callees", () => {
    test("constructors forwarding their arguments with apply", () => {
        const Class = {
            create() {
                return function () {
                    this.initialize.apply(this, arguments);
                };
            },
        };
        const Point = Class.create();
        Point.prototype = {
            initialize(x, y) {
                this.x = x;
                this.y = y;
            },
        };
        const Color = Class.create();
        Color.prototype = {
            initialize(r, g, b) {
                this.r = r;
                this.g = g;
                this.b = b;
            },
        };
        const Pair = Class.create();
        Pair.prototype = {
            initialize() {
                this.count = arguments.length;
                this.first = arguments[0];
            },
        };
        function make(i) {
            const point = new Point(i, 2);
            const color = new Color(1, i, 3);
            const pair = new Pair(i, i);
            return point.x + point.y + color.g + pair.count + pair.first;
        }
        for (let i = 0; i < iterations; ++i) expect(make(i)).toBe(3 * i + 4);
        // Another type of value.
        expect(make("a")).toBe("a2a2a");
        // A method that changes after compilation.
        Point.prototype.initialize = function (x, y) {
            this.x = x * 10;
            this.y = y;
        };
        expect(make(1)).toBe(16);
    });

    test("length and indices", () => {
        function count() {
            return arguments.length;
        }
        function second() {
            return arguments[1];
        }
        function outOfRange() {
            return arguments[3];
        }
        function caller(i) {
            return count(i, i, i) + second(1, i) + (outOfRange(i) === undefined ? 1 : 0);
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(i + 4);
    });

    test("exits that create the arguments object of an inlined callee", () => {
        function sum() {
            let total = 0;
            for (let i = 0; i < arguments.length; ++i) total += arguments[i];
            return total;
        }
        function inner(a, b) {
            const value = a + b;
            // A string makes this exit after the arguments were used.
            return value + Array.prototype.slice.call(arguments).length;
        }
        function caller(i) {
            return sum(i, 1, 2) + inner(i, 1, 2);
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(2 * i + 7);
        expect(inner("a", "b", "c")).toBe("ab3");
        expect(caller("x")).toBe("0x12x13");
    });

    test("returned arguments objects", () => {
        function args() {
            return arguments;
        }
        function caller(i) {
            const object = args(i, i + 1);
            return object.length + object[1] + (object !== arguments ? 1 : 0);
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(i + 4);
    });

    test("arguments that escape", () => {
        let kept;
        function keep() {
            kept = arguments;
            return arguments.length;
        }
        function caller(i) {
            return keep(i, i);
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(i)).toBe(2);
            expect(kept[0]).toBe(i);
        }
    });
});

describe("methods of objects of known shapes", () => {
    test("a method of a constructor many classes share", () => {
        function makeClass(scale) {
            const constructor = function (value) {
                this.initialize(value);
            };
            constructor.prototype = {
                initialize(value) {
                    this.value = value * scale;
                },
            };
            return constructor;
        }
        const classes = [];
        for (let scale = 1; scale <= 8; ++scale) classes.push(makeClass(scale));
        const [First, Second] = classes;
        function make(i) {
            return new First(i).value + new Second(i).value;
        }
        for (let i = 0; i < iterations; ++i) {
            for (const Class of classes) expect(new Class(i).value).toBeGreaterThanOrEqual(i);
            expect(make(i)).toBe(3 * i);
        }
        First.prototype.initialize = function (value) {
            this.value = -value;
        };
        expect(make(2)).toBe(2);
    });

    test("properties added before a read", () => {
        function Box(value) {
            this.value = value;
            this.double = this.twice();
        }
        Box.prototype.twice = function () {
            return this.value * 2;
        };
        function make(i) {
            const box = new Box(i);
            box.extra = 1;
            return box.double + box.extra;
        }
        for (let i = 0; i < iterations; ++i) expect(make(i)).toBe(2 * i + 1);
        Box.prototype.twice = function () {
            return 0;
        };
        expect(make(5)).toBe(1);
    });
});
