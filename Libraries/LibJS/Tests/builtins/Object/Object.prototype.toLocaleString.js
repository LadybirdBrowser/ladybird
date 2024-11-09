describe("correct behavior", () => {
    test("length", () => {
        expect(Object.prototype.toLocaleString).toHaveLength(0);
    });

    test("basic functionality", () => {
        let o;

        o = {};
        expect(o.toString()).toBe(o.toLocaleString());

        o = { toString: () => 42 };
        expect(o.toString()).toBe(42);
    });
});

describe("errors", () => {
    test("toString that throws error", () => {
        let o = {
            toString: () => {
                throw new Error();
            },
        };
        expect(() => {
            o.toLocaleString();
        }).toThrow(Error);
    });

    test("toString that is not a function", () => {
        let o = { toString: "foo" };
        expect(() => {
            o.toLocaleString();
        }).toThrowWithMessage(TypeError, "foo is not a function");
    });
});
