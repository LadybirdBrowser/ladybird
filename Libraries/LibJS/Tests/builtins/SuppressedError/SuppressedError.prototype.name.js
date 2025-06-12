describe("normal behavior", () => {
    test("initial name value is type name", () => {
        expect(SuppressedError.prototype.name).toBe("SuppressedError");
    });

    test("Error gets name via prototype", () => {
        const error = new SuppressedError([]);
        expect(error.hasOwnProperty("name")).toBeFalse();
        expect(error.name).toBe("SuppressedError");
        SuppressedError.prototype.name = "Foo";
        expect(error.name).toBe("Foo");
    });
});
