test("basic functionality", () => {
    const local = Symbol("foo");
    const global = Symbol.for("foo");
    expect(local.valueOf()).toBe(local);
    expect(global.valueOf()).toBe(global);

    expect(Symbol.prototype.valueOf.call(local)).toBe(local);
    expect(Symbol.prototype.valueOf.call(global)).toBe(global);
});

test("|this| must be a symbol", () => {
    expect(() => {
        Symbol.prototype.valueOf.call("foo");
    }).toThrowWithMessage(TypeError, "Not an object of type Symbol");
});
