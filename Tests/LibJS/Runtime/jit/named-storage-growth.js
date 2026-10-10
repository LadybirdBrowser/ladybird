// JIT code grows the named property storage of objects that properties are
// added to. These tests run every function many times, so that with a low JIT
// threshold they are compiled, and check every property afterwards.

const iterations = 100;

describe("named property storage growth in JIT code", () => {
    test("properties added one at a time, by name", () => {
        function build(seed) {
            const object = {};
            object.a = seed;
            object.b = seed + 1;
            object.c = seed + 2;
            object.d = seed + 3;
            object.e = seed + 4;
            object.f = seed + 5;
            object.g = seed + 6;
            object.h = seed + 7;
            object.i = seed + 8;
            object.j = seed + 9;
            object.k = seed + 10;
            object.l = seed + 11;
            object.m = seed + 12;
            object.n = seed + 13;
            object.o = seed + 14;
            object.p = seed + 15;
            object.q = seed + 16;
            object.r = seed + 17;
            return object;
        }
        for (let i = 0; i < iterations; ++i) {
            const object = build(i);
            const keys = Object.keys(object);
            expect(keys).toHaveLength(18);
            keys.forEach((key, index) => expect(object[key]).toBe(i + index));
        }
    });

    test("properties added by key, in a loop", () => {
        const keys = [];
        for (let i = 0; i < 40; ++i) keys.push("key" + i);
        function build(count) {
            const object = { first: "first" };
            for (let i = 0; i < count; ++i) object[keys[i]] = { index: i };
            return object;
        }
        for (let i = 0; i < iterations; ++i) {
            const count = i % 40;
            const object = build(count);
            expect(Object.keys(object)).toHaveLength(count + 1);
            expect(object.first).toBe("first");
            for (let j = 0; j < count; ++j) expect(object[keys[j]].index).toBe(j);
        }
    });

    test("objects of every inline capacity", () => {
        function grow(object, value) {
            object.added1 = value;
            object.added2 = value;
            object.added3 = value;
            return object;
        }
        for (let i = 0; i < iterations; ++i) {
            const objects = [
                {},
                { a: 1 },
                { a: 1, b: 2 },
                { a: 1, b: 2, c: 3, d: 4 },
                { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6 },
                { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8 },
            ];
            for (const object of objects) {
                const before = Object.entries(object);
                grow(object, i);
                expect(Object.entries(object)).toEqual([...before, ["added1", i], ["added2", i], ["added3", i]]);
            }
        }
    });

    test("objects stay intact across collections", () => {
        function build(n) {
            const object = {};
            for (let i = 0; i < n; ++i) {
                object["p" + i] = "value " + i;
                if (i % 7 === 6) gc();
            }
            return object;
        }
        for (let i = 0; i < 20; ++i) {
            const object = build(30);
            for (let j = 0; j < 30; ++j) expect(object["p" + j]).toBe("value " + j);
        }
    });
});
