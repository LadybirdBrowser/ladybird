// JIT code calls Map.prototype.get and Map.prototype.has through runtime helpers, without a native frame. The helpers
// decline for receivers that are no Map, which the builtins then throw for.
//
// NB: The functions come from new Function, so that each run of a test (see LIBJS_TEST_REPEAT) compiles new ones.

function compiled(parameters, body, warm_up) {
    const f = new Function(...parameters, body);
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) warm_up(f);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

describe("Map lookups in compiled code", () => {
    test("get and has find what set stored, with canonicalized keys", () => {
        const map = new Map([
            [1, "one"],
            ["a", "letter"],
            [0, "zero"],
            [NaN, "nan"],
        ]);
        const object = {};
        map.set(object, "object");
        const get = compiled(["map", "key"], "return map.get(key);", f => f(map, 1));
        const has = compiled(["map", "key"], "return map.has(key);", f => f(map, 1));
        expect(get(map, 1)).toBe("one");
        expect(get(map, 1.0)).toBe("one");
        expect(get(map, "1")).toBeUndefined();
        expect(get(map, "a")).toBe("letter");
        expect(get(map, -0)).toBe("zero");
        expect(get(map, NaN)).toBe("nan");
        expect(get(map, object)).toBe("object");
        expect(get(map, {})).toBeUndefined();
        expect(get(map)).toBeUndefined();
        expect(has(map, 1)).toBeTrue();
        expect(has(map, -0)).toBeTrue();
        expect(has(map, NaN)).toBeTrue();
        expect(has(map, 2)).toBeFalse();
        expect(has(map, object)).toBeTrue();
        expect(has(map)).toBeFalse();
    });

    test("lookups see changes between them", () => {
        const lookups = compiled(
            ["map"],
            "const results = [map.get(1), map.has(1)]; map.set(1, 'changed'); results.push(map.get(1)); map.delete(1); results.push(map.get(1), map.has(1)); return results;",
            f => f(new Map([[1, "first"]]))
        );
        expect(lookups(new Map([[1, "first"]]))).toEqual(["first", true, "changed", undefined, false]);
        expect(lookups(new Map())).toEqual([undefined, false, "changed", undefined, false]);
    });

    test("subclass instances are maps", () => {
        class CountingMap extends Map {}
        const map = new CountingMap([["x", 1]]);
        const get = compiled(["map", "key"], "return map.get(key);", f => f(new Map([["x", 2]]), "x"));
        expect(get(map, "x")).toBe(1);
        expect(get(map, "y")).toBeUndefined();
    });

    test("receivers that are no Map throw like the builtins", () => {
        const get = compiled(["map", "key"], "return Map.prototype.get.call(map, key);", f => f(new Map(), 1));
        const has = compiled(["map", "key"], "return Map.prototype.has.call(map, key);", f => f(new Map(), 1));
        for (const receiver of [new Set([1]), new WeakMap(), {}, 1, "map", undefined, null]) {
            expect(() => get(receiver, 1)).toThrow(TypeError);
            expect(() => has(receiver, 1)).toThrow(TypeError);
        }
        expect(get(new Map([[1, "one"]]), 1)).toBe("one");
        expect(has(new Map([[1, "one"]]), 1)).toBeTrue();
    });

    test("replaced methods are called", () => {
        const get = compiled(["map", "key"], "return map.get(key);", f => f(new Map(), 1));
        const map = new Map([[1, "one"]]);
        map.get = key => `replaced ${key}`;
        expect(get(map, 1)).toBe("replaced 1");
        expect(get(new Map([[1, "one"]]), 1)).toBe("one");
    });
});
