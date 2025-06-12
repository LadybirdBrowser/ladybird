test("basic functionality", () => {
    expect(RegExp.prototype.source).toBe("(?:)");
    expect(RegExp().source).toBe("(?:)");
    expect(/test/.source).toBe("test");
    expect(/\n/.source).toBe("\\n");
    expect(/foo\/bar/.source).toBe("foo\\/bar");
});
