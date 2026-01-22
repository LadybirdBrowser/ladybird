describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Function.prototype).toHaveLength(0);
    });

    test("name is empty string", () => {
        expect(Function.prototype.name).toBe("");
    });

    test("basic functionality", () => {
        function fn() {}
        expect(Object.getPrototypeOf(fn)).toBe(Function.prototype);
        expect(Object.getPrototypeOf(Function.prototype)).toBe(Object.prototype);
    });

    test("is callable", () => {
        expect(Function.prototype()).toBeUndefined();
    });

    test("is not constructable", () => {
        expect(() => new Function.prototype()).toThrowWithMessage(
            TypeError,
            "[object FunctionPrototype] is not a constructor (evaluated from 'Function.prototype')"
        );
    });
});
