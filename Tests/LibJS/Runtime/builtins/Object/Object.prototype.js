test("basic functionality", () => {
    var o = new Object();
    Object.prototype.foo = 123;
    expect(o.foo).toBe(123);
});

test("is an immutable prototype exotic object", () => {
    const p = Object.create(null);
    expect(() => {
        Object.setPrototypeOf(Object.prototype, p);
    }).toThrowWithMessage(TypeError, "Object's [[SetPrototypeOf]] method returned false");
});
