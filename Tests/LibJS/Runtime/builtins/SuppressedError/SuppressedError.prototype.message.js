describe("normal behavior", () => {
    test("initial message value is empty string", () => {
        expect(SuppressedError.prototype.message).toBe("");
    });

    test("Error gets message via prototype by default", () => {
        const error = new SuppressedError();
        expect(error.hasOwnProperty("message")).toBeFalse();
        expect(error.message).toBe("");
        SuppressedError.prototype.message = "Well hello friends";
        expect(error.message).toBe("Well hello friends");
    });

    test("Error gets message via object if given to constructor", () => {
        const error = new SuppressedError(undefined, undefined, "Custom error message");
        expect(error.hasOwnProperty("message")).toBeTrue();
        expect(error.message).toBe("Custom error message");
        SuppressedError.prototype.message = "Well hello friends";
        expect(error.message).toBe("Custom error message");
    });
});
