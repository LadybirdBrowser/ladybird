test("length is 1", () => {
    expect(Object.isFrozen).toHaveLength(1);
});

describe("normal behavior", () => {
    test("returns true for non-object argument", () => {
        expect(Object.isFrozen(42)).toBeTrue();
        expect(Object.isFrozen("foobar")).toBeTrue();
    });

    test("returns false for regular object", () => {
        const o = { foo: "bar" };
        expect(Object.isFrozen(o)).toBeFalse();
    });

    test("returns true for frozen object", () => {
        const o = { foo: "bar" };
        Object.freeze(o);
        expect(Object.isFrozen(o)).toBeTrue();
    });

    test("returns true for non-extensible empty object", () => {
        const o = {};
        Object.preventExtensions(o);
        expect(Object.isFrozen(o)).toBeTrue();
    });
});
