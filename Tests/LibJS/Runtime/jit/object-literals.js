// JIT code allocates the objects of object literals itself, with the shape the
// literal had before, and stores their properties straight into place. These
// tests run every function many times, so that with a low JIT threshold they
// are compiled after their literals cached a shape.

const iterations = 100;

describe("object literals in JIT code", () => {
    test("properties in order, empty literals and nested literals", () => {
        function make(a, b) {
            return { x: a, y: b, sum: a + b };
        }
        function empty() {
            return {};
        }
        function nested(i) {
            return { name: "if", hash: {}, data: i, loc: { start: { line: i, column: 2 }, end: { line: 3 } } };
        }
        for (let i = 0; i < iterations; ++i) {
            const object = make(i, 2);
            expect(Object.keys(object)).toEqual(["x", "y", "sum"]);
            expect(object.sum).toBe(i + 2);
            const blank = empty();
            expect(Object.keys(blank)).toEqual([]);
            expect(Object.getPrototypeOf(blank)).toBe(Object.prototype);
            blank.added = i;
            expect(blank.added).toBe(i);
            const tree = nested(i);
            expect(tree.loc.start.line).toBe(i);
            expect(tree.loc.end.line).toBe(3);
            expect(tree.hash).not.toBe(nested(i).hash);
        }
    });

    test("duplicate keys and literals with more properties than fit inline", () => {
        function duplicate(i) {
            return { a: 1, b: 2, a: i };
        }
        function wide(i) {
            return {
                p0: i,
                p1: i + 1,
                p2: i + 2,
                p3: i + 3,
                p4: i + 4,
                p5: i + 5,
                p6: i + 6,
                p7: i + 7,
                p8: i + 8,
                p9: i + 9,
                p10: i + 10,
                p11: i + 11,
                p12: i + 12,
                p13: i + 13,
                p14: i + 14,
                p15: i + 15,
                p16: i + 16,
                p17: i + 17,
            };
        }
        for (let i = 0; i < iterations; ++i) {
            const object = duplicate(i);
            expect(Object.keys(object)).toEqual(["a", "b"]);
            expect(object.a).toBe(i);
            const big = wide(i);
            expect(Object.keys(big).length).toBe(18);
            for (let j = 0; j < 18; ++j) expect(big["p" + j]).toBe(i + j);
            big.extra = 1;
            expect(big.extra).toBe(1);
        }
    });

    test("calls, collections and exceptions while a literal is initialized", () => {
        function value(i) {
            gc();
            if (i === 77) throw new Error("boom");
            return [i];
        }
        function make(i) {
            return { before: i, during: value(i), after: { i } };
        }
        for (let i = 0; i < iterations; ++i) {
            if (i === 77) {
                expect(() => make(i)).toThrowWithMessage(Error, "boom");
                continue;
            }
            const object = make(i);
            expect(object.before).toBe(i);
            expect(object.during[0]).toBe(i);
            expect(object.after.i).toBe(i);
        }
    });

    test("methods, getters and later changes to the objects", () => {
        const base = {
            greet() {
                return "base";
            },
        };
        function make(i) {
            const object = {
                value: i,
                greet() {
                    return super.greet() + this.value;
                },
            };
            Object.setPrototypeOf(object, base);
            return object;
        }
        function withGetter(i) {
            return {
                value: i,
                get twice() {
                    return this.value * 2;
                },
            };
        }
        for (let i = 0; i < iterations; ++i) {
            const object = make(i);
            expect(object.greet()).toBe("base" + i);
            delete object.value;
            expect(object.value).toBeUndefined();
            expect(withGetter(i).twice).toBe(2 * i);
            const frozen = Object.freeze({ a: i, b: i });
            expect(Object.isFrozen(frozen)).toBeTrue();
            expect(Object.isFrozen({ a: i, b: i })).toBeFalse();
        }
    });

    test("literals whose shape changes between runs", () => {
        function make(i, spread) {
            return { a: i, ...spread, b: i };
        }
        for (let i = 0; i < iterations; ++i) {
            const object = make(i, i % 2 ? { c: 1 } : {});
            expect(object.a).toBe(i);
            expect(object.b).toBe(i);
            expect(object.c).toBe(i % 2 ? 1 : undefined);
        }
    });
});
