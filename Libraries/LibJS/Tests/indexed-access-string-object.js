test("string literal indexing", () => {
    var s = "foo";
    expect(s[0]).toBe("f");
    expect(s[1]).toBe("o");
    expect(s[2]).toBe("o");
    expect(s[3]).toBeUndefined();
});

test("string object indexing", () => {
    var s = new String("bar");
    expect(s[0]).toBe("b");
    expect(s[1]).toBe("a");
    expect(s[2]).toBe("r");
    expect(s[3]).toBeUndefined();
});
