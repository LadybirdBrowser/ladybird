test("basic functionality", () => {
    expect(Object).toHaveLength(1);
    expect(Object.name).toBe("Object");
    expect(Object.prototype).not.toHaveProperty("length");

    expect(typeof Object()).toBe("object");
    expect(typeof new Object()).toBe("object");

    expect(typeof Object(42)).toBe("object");
    expect(Object(42).valueOf()).toBe(42);
    expect(typeof Object("foo")).toBe("object");
    expect(Object("foo").valueOf()).toBe("foo");
});
