describe("normal behavior", () => {
    test("length is 2", () => {
        expect(SuppressedError).toHaveLength(3);
    });

    test("name is SuppressedError", () => {
        expect(SuppressedError.name).toBe("SuppressedError");
    });

    test("Prototype of the SuppressedError constructor is the Error constructor", () => {
        expect(Object.getPrototypeOf(SuppressedError)).toBe(Error);
    });

    test("Prototype of SuppressedError.prototype is Error.prototype", () => {
        expect(Object.getPrototypeOf(SuppressedError.prototype)).toBe(Error.prototype);
    });

    test("construction", () => {
        expect(SuppressedError()).toBeInstanceOf(SuppressedError);
        expect(SuppressedError(1)).toBeInstanceOf(SuppressedError);
        expect(SuppressedError(1, 1)).toBeInstanceOf(SuppressedError);
        expect(new SuppressedError()).toBeInstanceOf(SuppressedError);
        expect(new SuppressedError(1)).toBeInstanceOf(SuppressedError);
        expect(new SuppressedError(1, 1)).toBeInstanceOf(SuppressedError);
        expect(Object.hasOwn(new SuppressedError(1, 1), "message")).toBeFalse();
        expect(new SuppressedError().toString()).toBe("SuppressedError");
        expect(new SuppressedError(1).toString()).toBe("SuppressedError");
        expect(new SuppressedError(1, 1).toString()).toBe("SuppressedError");
        expect(new SuppressedError(undefined, undefined, "Foo").toString()).toBe(
            "SuppressedError: Foo"
        );
        expect(new SuppressedError(1, 1, "Foo").toString()).toBe("SuppressedError: Foo");
        expect(Object.hasOwn(new SuppressedError(), "error")).toBeTrue();
        expect(Object.hasOwn(new SuppressedError(), "suppressed")).toBeTrue();
        const obj = {};
        expect(new SuppressedError(obj).error).toBe(obj);
        expect(new SuppressedError(null, obj).suppressed).toBe(obj);
    });

    test("converts message to string", () => {
        expect(new SuppressedError(undefined, undefined, 1)).toHaveProperty("message", "1");
        expect(new SuppressedError(undefined, undefined, {})).toHaveProperty(
            "message",
            "[object Object]"
        );
    });

    test("supports options object with cause", () => {
        const cause = new Error();
        const error = new SuppressedError(1, 2, "test", { cause });
        expect(error.hasOwnProperty("cause")).toBeTrue();
        expect(error.cause).toBe(cause);

        const errorWithoutCase = new SuppressedError(1, 2, "test");
        expect(errorWithoutCase.hasOwnProperty("cause")).toBeFalse();
    });
});
