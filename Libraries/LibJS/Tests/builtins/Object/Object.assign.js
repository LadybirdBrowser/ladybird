test("length is 2", () => {
    expect(Object.assign).toHaveLength(2);
});

describe("errors", () => {
    test("first argument must coercible to object", () => {
        expect(() => {
            Object.assign(null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
        expect(() => {
            Object.assign(undefined);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });
});

describe("normal behavior", () => {
    test("returns first argument coerced to object", () => {
        const o = {};
        expect(Object.assign(o)).toBe(o);
        expect(Object.assign(o, {})).toBe(o);
        expect(Object.assign(42)).toEqual(new Number(42));
    });

    test("alters first argument object if sources are given", () => {
        const o = { foo: 0 };
        expect(Object.assign(o, { foo: 1 })).toBe(o);
        expect(o).toEqual({ foo: 1 });
    });

    test("merges objects", () => {
        const s = Symbol();
        expect(
            Object.assign(
                {},
                { foo: 0, bar: "baz" },
                { [s]: [1, 2, 3] },
                { foo: 1 },
                { [42]: "test" }
            )
        ).toEqual({ foo: 1, bar: "baz", [s]: [1, 2, 3], 42: "test" });
    });
});
