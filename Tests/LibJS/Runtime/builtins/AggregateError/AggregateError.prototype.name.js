describe("normal behavior", () => {
    test("initial name value is type name", () => {
        expect(AggregateError.prototype.name).toBe("AggregateError");
    });

    test("Error gets name via prototype", () => {
        const error = new AggregateError([]);
        expect(error.hasOwnProperty("name")).toBeFalse();
        expect(error.name).toBe("AggregateError");
        AggregateError.prototype.name = "Foo";
        expect(error.name).toBe("Foo");
    });
});
