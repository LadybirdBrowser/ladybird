test("basic functionality", () => {
    var foo = true;
    expect(foo.valueOf()).toBeTrue();
    expect(true.valueOf()).toBeTrue();

    expect(Boolean.prototype.valueOf.call(true)).toBeTrue();
    expect(Boolean.prototype.valueOf.call(false)).toBeFalse();

    expect(new Boolean().valueOf()).toBeFalse();
});

test("errors on non-boolean |this|", () => {
    expect(() => {
        Boolean.prototype.valueOf.call("foo");
    }).toThrowWithMessage(TypeError, "Not an object of type Boolean");
});
