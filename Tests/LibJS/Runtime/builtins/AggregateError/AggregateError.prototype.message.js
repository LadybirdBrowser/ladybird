describe("normal behavior", () => {
    test("initial message value is empty string", () => {
        expect(AggregateError.prototype.message).toBe("");
    });

    test("Error gets message via prototype by default", () => {
        const error = new AggregateError([]);
        expect(error.hasOwnProperty("message")).toBeFalse();
        expect(error.message).toBe("");
        AggregateError.prototype.message = "Well hello friends";
        expect(error.message).toBe("Well hello friends");
    });

    test("Error gets message via object if given to constructor", () => {
        const error = new AggregateError([], "Custom error message");
        expect(error.hasOwnProperty("message")).toBeTrue();
        expect(error.message).toBe("Custom error message");
        AggregateError.prototype.message = "Well hello friends";
        expect(error.message).toBe("Custom error message");
    });
});
