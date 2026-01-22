test("constructor properties", () => {
    expect(WeakRef).toHaveLength(1);
    expect(WeakRef.name).toBe("WeakRef");
});

describe("errors", () => {
    test("invalid array iterators", () => {
        [-100, Infinity, NaN, 152n, undefined].forEach(value => {
            expect(() => {
                new WeakRef(value);
            }).toThrowWithMessage(TypeError, "cannot be held weakly");
        });
    });
    test("called without new", () => {
        expect(() => {
            WeakRef();
        }).toThrowWithMessage(TypeError, "WeakRef constructor must be called with 'new'");
    });
});

describe("normal behavior", () => {
    test("typeof", () => {
        expect(typeof new WeakRef({})).toBe("object");
    });

    test("constructor with single object argument", () => {
        var a = new WeakRef({});
        expect(a instanceof WeakRef).toBeTrue();
    });

    test("constructor with single symbol argument", () => {
        var a = new WeakRef(Symbol());
        expect(a instanceof WeakRef).toBeTrue();
    });
});
