test("basic functionality", () => {
    expect(ShadowRealm.prototype[Symbol.toStringTag]).toBe("ShadowRealm");
});
