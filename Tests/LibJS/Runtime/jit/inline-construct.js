// JIT code inlines constructs of base constructors. These tests run every
// function many times, so that with a low JIT threshold they are compiled,
// and then make the inlined code exit or throw.

const iterations = 100;

describe("inlined constructs", () => {
    test("objects get their prototype and properties", () => {
        function Point(x, y) {
            this.x = x;
            this.y = y;
        }
        Point.prototype.sum = function () {
            return this.x + this.y;
        };
        function make(i) {
            return new Point(i, i + 1);
        }
        const seen = new Set();
        for (let i = 0; i < iterations; ++i) {
            const point = make(i);
            expect(point).toBeInstanceOf(Point);
            expect(Object.getPrototypeOf(point)).toBe(Point.prototype);
            expect(point.constructor).toBe(Point);
            expect(Object.keys(point)).toEqual(["x", "y"]);
            expect(point.sum()).toBe(2 * i + 1);
            expect(seen.has(point)).toBeFalse();
            seen.add(point);
        }
    });

    test("constructors that return this or a primitive result in this", () => {
        function ReturnsThis(value) {
            this.value = value;
            return this;
        }
        function ReturnsPrimitive(value) {
            this.value = value;
            return 42;
        }
        function make(i) {
            return [new ReturnsThis(i), new ReturnsPrimitive(i)];
        }
        for (let i = 0; i < iterations; ++i) {
            const [a, b] = make(i);
            expect(a).toBeInstanceOf(ReturnsThis);
            expect(a.value).toBe(i);
            expect(b).toBeInstanceOf(ReturnsPrimitive);
            expect(b.value).toBe(i);
        }
    });

    test("constructors that return objects", () => {
        function Replaced(value) {
            this.ignored = true;
            if (value > 1000) return { replaced: value };
        }
        function make(i) {
            return new Replaced(i);
        }
        for (let i = 0; i < iterations; ++i) expect(make(i).ignored).toBeTrue();
        expect(make(2000)).toEqual({ replaced: 2000 });
    });

    test("a new prototype is seen", () => {
        function Thing(value) {
            this.value = value;
        }
        function make(i) {
            return new Thing(i);
        }
        for (let i = 0; i < iterations; ++i) expect(Object.getPrototypeOf(make(i))).toBe(Thing.prototype);
        const original = Thing.prototype;
        const replacement = { replacement: true };
        Thing.prototype = replacement;
        const thing = make(1);
        expect(Object.getPrototypeOf(thing)).toBe(replacement);
        expect(thing.replacement).toBeTrue();
        expect(thing.value).toBe(1);
        Thing.prototype = original;
        expect(Object.getPrototypeOf(make(2))).toBe(original);
    });

    test("exits and exceptions inside inlined constructors", () => {
        function Sum(a, b) {
            this.first = a;
            this.sum = a + b;
            this.last = b;
        }
        function Throws(value) {
            this.value = value;
            if (value.missing.property) this.never = true;
        }
        function make(a, b) {
            return new Sum(a, b);
        }
        function makeThrowing(value) {
            try {
                return new Throws(value);
            } catch (error) {
                return error;
            }
        }
        for (let i = 0; i < iterations; ++i) {
            expect(make(i, 1).sum).toBe(i + 1);
            expect(makeThrowing({ missing: {} }).value).toEqual({ missing: {} });
        }
        const strings = make("a", "b");
        expect(strings).toBeInstanceOf(Sum);
        expect(strings).toEqual({ first: "a", sum: "ab", last: "b" });
        expect(makeThrowing({})).toBeInstanceOf(TypeError);
    });

    test("classes", () => {
        class Vector {
            constructor(x) {
                this.x = x;
            }
            get double() {
                return this.x * 2;
            }
        }
        function make(i) {
            return new Vector(i);
        }
        for (let i = 0; i < iterations; ++i) {
            const vector = make(i);
            expect(vector).toBeInstanceOf(Vector);
            expect(vector.double).toBe(2 * i);
        }
    });

    test("collections during inlined constructs", () => {
        function Node(value, next) {
            this.value = value;
            this.next = next;
            if (value % 50 === 49) gc();
        }
        function build(count) {
            let list = null;
            for (let i = 0; i < count; ++i) list = new Node(i, list);
            return list;
        }
        for (let i = 0; i < 20; ++i) {
            let list = build(100);
            for (let value = 99; value >= 0; --value, list = list.next) expect(list.value).toBe(value);
            expect(list).toBeNull();
        }
    });

    test("properties added to constructed objects", () => {
        function Pair(first, second) {
            this.first = first;
            this.second = second;
        }
        function sum(first, second) {
            const pair = new Pair(first, second);
            return pair.first + pair.second;
        }
        function keep(first, second, reveal) {
            const pair = new Pair(first, second);
            const total = pair.first + pair.second;
            if (reveal) return reveal(pair);
            return total;
        }
        for (let i = 0; i < iterations; ++i) {
            expect(sum(i, 1)).toBe(i + 1);
            expect(keep(i, 2, null)).toBe(i + 2);
        }
        const pair = keep(3, 4, object => object);
        expect(pair).toBeInstanceOf(Pair);
        expect(Object.keys(pair)).toEqual(["first", "second"]);
        expect(pair.first).toBe(3);
        expect(pair.second).toBe(4);
        pair.third = 5;
        expect(Object.keys(pair)).toEqual(["first", "second", "third"]);
        expect(sum("a", "b")).toBe("ab");
    });

    test("setters in the prototype chain that appear later", () => {
        function Box(value) {
            this.value = value;
        }
        function make(value) {
            const box = new Box(value);
            return box.value;
        }
        for (let i = 0; i < iterations; ++i) expect(make(i)).toBe(i);
        let set = null;
        Object.defineProperty(Box.prototype, "value", {
            set(value) {
                set = value;
            },
            get() {
                return "getter";
            },
            configurable: true,
        });
        expect(make(7)).toBe("getter");
        expect(set).toBe(7);
        delete Box.prototype.value;
        expect(make(8)).toBe(8);
    });

    test("objects with many properties", () => {
        function Wide(value) {
            this.a = value;
            this.b = value + 1;
            this.c = value + 2;
            this.d = value + 3;
            this.e = value + 4;
            this.f = value + 5;
            this.g = value + 6;
            this.h = value + 7;
            this.i = value + 8;
            this.j = value + 9;
        }
        function make(value) {
            return new Wide(value);
        }
        for (let i = 0; i < iterations; ++i) {
            const wide = make(i);
            expect(Object.values(wide)).toEqual([i, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7, i + 8, i + 9]);
        }
    });

    test("constructors larger than inlined calls", () => {
        function Node(type, name) {
            this.type = type;
            this.name = name;
            this.tag = type === 1 ? name : undefined;
            this.lower = type === 1 ? name.toLowerCase() : undefined;
            this.parent = null;
            this.first = null;
            this.last = null;
            this.next = null;
            this.previous = null;
            this.owner = null;
            this.value = null;
            this.attributes = null;
            this.listeners = null;
            this.classes = null;
            this.style = null;
            this.className = "";
            this.id = "";
            this.width = 0;
            this.height = 0;
        }
        function make(i) {
            return new Node(i % 2, i % 2 ? "DIV" : "#text");
        }
        for (let i = 0; i < iterations; ++i) {
            const node = make(i);
            expect(Object.getPrototypeOf(node)).toBe(Node.prototype);
            expect(node.lower).toBe(i % 2 ? "div" : undefined);
            expect(Object.keys(node)).toHaveLength(19);
        }
        // The inlined constructor throws for a name without toLowerCase.
        expect(() => new Node(1, null)).toThrow(TypeError);
        expect(() => make({ valueOf: () => 1 })).not.toThrow();
    });
});
