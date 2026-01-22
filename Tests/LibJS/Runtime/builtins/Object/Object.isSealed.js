test("length is 1", () => {
    expect(Object.isSealed).toHaveLength(1);
});

describe("normal behavior", () => {
    test("returns true for non-object argument", () => {
        expect(Object.isSealed(42)).toBeTrue();
        expect(Object.isSealed("foobar")).toBeTrue();
    });

    test("returns false for regular object", () => {
        const o = { foo: "bar" };
        expect(Object.isSealed(o)).toBeFalse();
    });

    test("returns true for sealed object", () => {
        const o = { foo: "bar" };
        Object.seal(o);
        expect(Object.isSealed(o)).toBeTrue();
    });

    test("returns true for non-extensible empty object", () => {
        const o = {};
        Object.preventExtensions(o);
        expect(Object.isSealed(o)).toBeTrue();
    });
});
