test("basic functionality", () => {
    const s = Symbol();
    expect(s[Symbol.toPrimitive]("string")).toBe(s);
    expect(s[Symbol.toPrimitive]("number")).toBe(s);
});
