test("constructor properties", () => {
    expect(WeakSet).toHaveLength(0);
    expect(WeakSet.name).toBe("WeakSet");
});

describe("errors", () => {
    test("invalid array iterators", () => {
        [-100, Infinity, NaN, {}, 152n].forEach(value => {
            expect(() => {
                new WeakSet(value);
            }).toThrowWithMessage(TypeError, "is not iterable");
        });
    });
    test("called without new", () => {
        expect(() => {
            WeakSet();
        }).toThrowWithMessage(TypeError, "WeakSet constructor must be called with 'new'");
    });
});

describe("normal behavior", () => {
    test("typeof", () => {
        expect(typeof new WeakSet()).toBe("object");
    });

    test("constructor with single array argument", () => {
        var a = new WeakSet([{ a: 1 }, { a: 2 }, { a: 3 }, Symbol("foo")]);
        expect(a instanceof WeakSet).toBeTrue();
    });
});
