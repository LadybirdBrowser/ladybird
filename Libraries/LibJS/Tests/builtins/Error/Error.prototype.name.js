describe("normal behavior", () => {
    test("initial name value is type name", () => {
        expect(Error.prototype.name).toBe("Error");
        expect(EvalError.prototype.name).toBe("EvalError");
        expect(RangeError.prototype.name).toBe("RangeError");
        expect(ReferenceError.prototype.name).toBe("ReferenceError");
        expect(SyntaxError.prototype.name).toBe("SyntaxError");
        expect(TypeError.prototype.name).toBe("TypeError");
    });

    test("Error gets name via prototype", () => {
        const error = new Error();
        expect(error.hasOwnProperty("name")).toBeFalse();
        expect(error.name).toBe("Error");
        Error.prototype.name = "Foo";
        expect(error.name).toBe("Foo");
    });
});
