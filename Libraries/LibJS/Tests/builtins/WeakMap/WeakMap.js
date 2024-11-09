test("constructor properties", () => {
    expect(WeakMap).toHaveLength(0);
    expect(WeakMap.name).toBe("WeakMap");
});

describe("errors", () => {
    test("invalid array iterators", () => {
        [-100, Infinity, NaN, {}, 152n].forEach(value => {
            expect(() => {
                new WeakMap(value);
            }).toThrowWithMessage(TypeError, "is not iterable");
        });
    });
    test("called without new", () => {
        expect(() => {
            WeakMap();
        }).toThrowWithMessage(TypeError, "WeakMap constructor must be called with 'new'");
    });
});

describe("normal behavior", () => {
    test("typeof", () => {
        expect(typeof new WeakMap()).toBe("object");
    });

    test("constructor with single array of entries argument", () => {
        var a = new WeakMap([
            [{ a: 1 }, 1],
            [{ a: 2 }, 2],
            [{ a: 3 }, 3],
        ]);
        expect(a instanceof WeakMap).toBeTrue();
    });
});

describe("regressions", () => {
    test("missing key/value properties on iterable entry", () => {
        expect(() => {
            new WeakMap([{}]);
        }).toThrowWithMessage(TypeError, "undefined cannot be held weakly");
    });
});
