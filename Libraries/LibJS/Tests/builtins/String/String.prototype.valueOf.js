test("basic functionality", () => {
    expect(String.prototype.valueOf).toHaveLength(0);

    expect(String()).toBe("");
    expect(new String().valueOf()).toBe("");
    expect(String("foo")).toBe("foo");
    expect(new String("foo").valueOf()).toBe("foo");
    expect(String(123)).toBe("123");
    expect(new String(123).valueOf()).toBe("123");
    expect(String(123)).toBe("123");
    expect(new String(123).valueOf()).toBe("123");
});
