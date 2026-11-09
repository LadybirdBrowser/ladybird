// The sites of a function past the last one of a cache table share one slot. What one of them sees must never give
// another one a wrong value.

test("property lookups past the last cache slot", () => {
    const count = 70000;
    const f = new Function(
        "a",
        "b",
        "let s = 0;" + "s += a.x;".repeat(count) + "s += a.y; s += b.z; s += a.x; s += b.w; return s;"
    );
    for (let i = 0; i < 5; ++i) {
        expect(f({ x: 1, y: 2 }, { z: 3, w: 4 })).toBe(count + 1 + 2 + 3 + 4);
        expect(f({ y: 20, x: 10 }, { w: 40, z: 30 })).toBe(count * 10 + 10 + 20 + 30 + 40);
    }
});

test("dynamic global variable reads past the last cache slot", () => {
    globalThis.manyCacheSitesOne = 1;
    globalThis.manyCacheSitesTwo = 2;
    const count = 70000;
    const f = new Function(
        "let s = 0;" +
            "s += manyCacheSitesOne;".repeat(count) +
            "s += manyCacheSitesTwo; s += manyCacheSitesOne; return s;"
    );
    for (let i = 0; i < 5; ++i) {
        expect(f()).toBe(count + 2 + 1);
    }
    globalThis.manyCacheSitesTwo = 5;
    expect(f()).toBe(count + 5 + 1);
});

test("the shared slot outlives the functions that use it", () => {
    const count = 70000;
    const makeFunction = () => new Function("a", "let s = 0;" + "s += a.y;".repeat(count) + "return s;");
    for (let i = 0; i < 4; ++i) {
        expect(makeFunction()({ x: 1, y: 2 })).toBe(count * 2);
        gc();
    }
    const getX = new Function("o", "let s = 0;" + "s += o.x;".repeat(1000) + "return s;");
    for (let i = 0; i < 3; ++i) {
        expect(getX({ x: 1, y: 2 })).toBe(1000);
    }
    const f = makeFunction();
    for (let i = 0; i < 3; ++i) {
        expect(f({ x: 1, y: 2 })).toBe(count * 2);
    }
});
