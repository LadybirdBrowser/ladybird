test("basic functionality", () => {
    var foo = true;
    expect(foo.toString()).toBe("true");
    expect(true.toString()).toBe("true");

    expect(Boolean.prototype.toString.call(true)).toBe("true");
    expect(Boolean.prototype.toString.call(false)).toBe("false");

    expect(new Boolean(true).toString()).toBe("true");
    expect(new Boolean(false).toString()).toBe("false");
});

test("errors on non-boolean |this|", () => {
    expect(() => {
        Boolean.prototype.toString.call("foo");
    }).toThrowWithMessage(TypeError, "Not an object of type Boolean");
});
