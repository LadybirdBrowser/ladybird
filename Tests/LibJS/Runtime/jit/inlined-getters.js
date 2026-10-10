// JIT code inlines the getters of accessors that property gets call: those
// of prototypes, and those of the objects themselves that the interpreter
// saw. These tests run every function many times, so that with a low JIT
// threshold they are compiled.

const iterations = 100;

class Point {
    constructor(x, y) {
        this._x = x;
        this._y = y;
    }

    get x() {
        return this._x;
    }

    get sum() {
        return this._x + this._y;
    }
}

function getX(point) {
    return point.x;
}

function getSum(point) {
    return point.sum;
}

describe("Inlined getters in JIT code", () => {
    test("getters of a class", () => {
        for (let i = 0; i < iterations; ++i) {
            expect(getX(new Point(i, 1))).toBe(i);
            expect(getSum(new Point(i, 1))).toBe(i + 1);
        }
    });

    test("receivers of several shapes with the same prototype", () => {
        for (let i = 0; i < iterations; ++i) {
            const point = new Point(i, 2);
            if (i % 2) point.extra = true;
            if (i % 3) point.more = true;
            expect(getX(point)).toBe(i);
        }
    });

    test("values the getter did not see before", () => {
        for (let i = 0; i < iterations; ++i) expect(getSum(new Point(i, i))).toBe(2 * i);
        expect(getSum(new Point(0.5, 0.25))).toBe(0.75);
        expect(getSum(new Point("a", "b"))).toBe("ab");
        expect(getSum(new Point(2 ** 30, 2 ** 30))).toBe(2 ** 31);
        expect(getSum(new Point({ valueOf: () => 40 }, 2))).toBe(42);
        expect(getSum(new Point(1, 2))).toBe(3);
    });

    test("getters that throw", () => {
        class Thrower {
            get value() {
                if (this.fail) throw new Error("getter failed");
                return 1;
            }
        }
        function get(object) {
            return object.value;
        }
        for (let i = 0; i < iterations; ++i) expect(get(new Thrower())).toBe(1);
        const failing = new Thrower();
        failing.fail = true;
        expect(() => get(failing)).toThrowWithMessage(Error, "getter failed");
        expect(get(new Thrower())).toBe(1);
    });

    test("getters that change", () => {
        class Box {
            constructor(value) {
                this.value = value;
            }
            get doubled() {
                return this.value * 2;
            }
        }
        function get(box) {
            return box.doubled;
        }
        for (let i = 0; i < iterations; ++i) expect(get(new Box(i))).toBe(2 * i);

        Object.defineProperty(Box.prototype, "doubled", {
            get() {
                return this.value * 3;
            },
            configurable: true,
        });
        expect(get(new Box(5))).toBe(15);
        for (let i = 0; i < iterations; ++i) expect(get(new Box(i))).toBe(3 * i);

        Object.defineProperty(Box.prototype, "doubled", { value: "data", configurable: true, writable: true });
        expect(get(new Box(5))).toBe("data");

        const own = new Box(1);
        Object.defineProperty(own, "doubled", { value: "own" });
        expect(get(own)).toBe("own");
    });

    test("prototypes that change", () => {
        class Base {
            get name() {
                return "base";
            }
        }
        class Derived extends Base {}
        function get(object) {
            return object.name;
        }
        for (let i = 0; i < iterations; ++i) expect(get(new Derived())).toBe("base");
        Object.defineProperty(Derived.prototype, "name", {
            get() {
                return "derived";
            },
            configurable: true,
        });
        expect(get(new Derived())).toBe("derived");
        expect(get(new Base())).toBe("base");
    });

    test("this and stack traces in getters", () => {
        let receiver;
        let stack;
        class Recorder {
            get me() {
                "use strict";
                receiver = this;
                stack = new Error().stack;
                return this;
            }
        }
        function get(object) {
            return object.me;
        }
        for (let i = 0; i < iterations; ++i) {
            const recorder = new Recorder();
            expect(get(recorder)).toBe(recorder);
            expect(receiver).toBe(recorder);
        }
        expect(stack.includes("get me")).toBeTrue();
    });

    test("recursive getters", () => {
        class Node {
            constructor(parent) {
                this.parent = parent;
            }
            get depth() {
                return this.parent ? this.parent.depth + 1 : 0;
            }
        }
        let node = new Node(null);
        for (let i = 0; i < 50; ++i) node = new Node(node);
        for (let i = 0; i < iterations; ++i) expect(node.depth).toBe(50);
    });

    test("own accessors, like the exports of bundled modules", () => {
        const exports = {};
        let binding = 1;
        Object.defineProperty(exports, "value", { enumerable: true, configurable: true, get: () => binding });
        Object.defineProperty(exports, "other", { enumerable: true, get: () => binding * 10 });
        function getValue(module) {
            return module.value;
        }
        for (let i = 0; i < iterations; ++i) {
            binding = i;
            expect(getValue(exports)).toBe(i);
            expect(exports.other).toBe(i * 10);
        }
        Object.defineProperty(exports, "value", { get: () => "replaced" });
        expect(getValue(exports)).toBe("replaced");
    });

    test("own accessors of objects of the same shape with other getters", () => {
        function make(value) {
            const object = {};
            Object.defineProperty(object, "value", { get: () => value, configurable: true });
            return object;
        }
        function get(object) {
            return object.value;
        }
        const first = make("first");
        for (let i = 0; i < iterations; ++i) expect(get(first)).toBe("first");
        for (let i = 0; i < iterations; ++i) expect(get(make(i))).toBe(i);
        const data = make(0);
        Object.defineProperty(data, "value", { value: "data" });
        expect(get(data)).toBe("data");
        expect(get(first)).toBe("first");
    });

    test("getters of classes that one function makes", () => {
        function makeClass(scale) {
            return class {
                constructor(value) {
                    this._value = value;
                }

                get scaled() {
                    return this._value * scale;
                }
            };
        }
        function get(object) {
            return object.scaled;
        }
        const classes = [makeClass(1), makeClass(10), makeClass(100)];
        for (let i = 0; i < iterations; ++i) {
            for (let j = 0; j < classes.length; ++j) expect(get(new classes[j](i))).toBe(i * 10 ** j);
        }
        const other = makeClass(2);
        Object.defineProperty(other.prototype, "scaled", { get: () => "replaced" });
        expect(get(new other(1))).toBe("replaced");
        expect(get(new classes[2](3))).toBe(300);
    });
});
