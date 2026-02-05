describe("normal behavior", () => {
    test("initial message value is empty string", () => {
        expect(Error.prototype.message).toBe("");
        expect(EvalError.prototype.message).toBe("");
        expect(RangeError.prototype.message).toBe("");
        expect(ReferenceError.prototype.message).toBe("");
        expect(SyntaxError.prototype.message).toBe("");
        expect(TypeError.prototype.message).toBe("");
    });

    test("Error gets message via prototype by default", () => {
        const error = new Error();
        expect(error.hasOwnProperty("message")).toBeFalse();
        expect(error.message).toBe("");
        Error.prototype.message = "Well hello friends";
        expect(error.message).toBe("Well hello friends");
    });

    test("Error gets message via object if given to constructor", () => {
        const error = new Error("Custom error message");
        expect(error.hasOwnProperty("message")).toBeTrue();
        expect(error.message).toBe("Custom error message");
        Error.prototype.message = "Well hello friends";
        expect(error.message).toBe("Custom error message");
    });
});
