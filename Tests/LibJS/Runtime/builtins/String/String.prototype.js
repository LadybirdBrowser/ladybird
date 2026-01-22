test("basic functionality", () => {
    expect(String.prototype).toHaveLength(0);

    expect(typeof Object.getPrototypeOf("")).toBe("object");
    expect(Object.getPrototypeOf("").valueOf()).toBe("");

    expect(typeof String.prototype).toBe("object");
    expect(String.prototype.valueOf()).toBe("");
});
