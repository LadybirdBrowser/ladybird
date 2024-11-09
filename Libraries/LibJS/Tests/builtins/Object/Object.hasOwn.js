describe("basic functionality", () => {
    test("length", () => {
        expect(Object.hasOwn).toHaveLength(2);
    });

    test("returns true for existent own property", () => {
        const o = { foo: "bar" };
        expect(Object.hasOwn(o, "foo")).toBeTrue();
    });

    test("returns false for non-existent own property", () => {
        const o = {};
        expect(Object.hasOwn(o, "foo")).toBeFalse();
    });

    test("returns false for existent prototype chain property", () => {
        const o = {};
        Object.prototype.foo = "bar";
        expect(Object.hasOwn(o, "foo")).toBeFalse();
    });
});

describe("errors", () => {
    test("null argument", () => {
        expect(() => {
            Object.hasOwn(null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("undefined argument", () => {
        expect(() => {
            Object.hasOwn(undefined);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });
});
