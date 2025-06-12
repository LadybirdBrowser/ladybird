test("basic functionality", () => {
    expect(JSON[Symbol.toStringTag]).toBe("JSON");
    expect(JSON.toString()).toBe("[object JSON]");
});
