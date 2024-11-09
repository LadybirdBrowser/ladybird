describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Error).toHaveLength(1);
        expect(EvalError).toHaveLength(1);
        expect(RangeError).toHaveLength(1);
        expect(ReferenceError).toHaveLength(1);
        expect(SyntaxError).toHaveLength(1);
        expect(TypeError).toHaveLength(1);
    });

    test("name matches constructor name", () => {
        expect(Error.name).toBe("Error");
        expect(EvalError.name).toBe("EvalError");
        expect(RangeError.name).toBe("RangeError");
        expect(ReferenceError.name).toBe("ReferenceError");
        expect(SyntaxError.name).toBe("SyntaxError");
        expect(TypeError.name).toBe("TypeError");
    });

    test("basic functionality", () => {
        expect(Error()).toBeInstanceOf(Error);
        expect(new Error()).toBeInstanceOf(Error);
        expect(EvalError()).toBeInstanceOf(EvalError);
        expect(new EvalError()).toBeInstanceOf(EvalError);
        expect(RangeError()).toBeInstanceOf(RangeError);
        expect(new RangeError()).toBeInstanceOf(RangeError);
        expect(ReferenceError()).toBeInstanceOf(ReferenceError);
        expect(new ReferenceError()).toBeInstanceOf(ReferenceError);
        expect(SyntaxError()).toBeInstanceOf(SyntaxError);
        expect(new SyntaxError()).toBeInstanceOf(SyntaxError);
        expect(TypeError()).toBeInstanceOf(TypeError);
        expect(new TypeError()).toBeInstanceOf(TypeError);
    });

    test("supports options object with cause", () => {
        const errors = [Error, EvalError, RangeError, ReferenceError, SyntaxError, TypeError];
        const cause = new Error();
        errors.forEach(T => {
            const error = new T("test", { cause });
            expect(error.hasOwnProperty("cause")).toBeTrue();
            expect(error.cause).toBe(cause);
        });
    });

    test("supports options object with cause (chained)", () => {
        let error;
        try {
            try {
                throw new Error("foo");
            } catch (e) {
                throw new Error("bar", { cause: e });
            }
        } catch (e) {
            error = new Error("baz", { cause: e });
        }
        expect(error.message).toBe("baz");
        expect(error.cause.message).toBe("bar");
        expect(error.cause.cause.message).toBe("foo");
        expect(error.cause.cause.cause).toBe(undefined);
    });
});
