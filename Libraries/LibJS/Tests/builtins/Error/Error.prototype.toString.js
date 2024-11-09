test("basic functionality", () => {
    expect(Error().toString()).toBe("Error");
    expect(Error(undefined).toString()).toBe("Error");
    expect(Error(null).toString()).toBe("Error: null");
    expect(Error("test").toString()).toBe("Error: test");
    expect(Error(42).toString()).toBe("Error: 42");
});
