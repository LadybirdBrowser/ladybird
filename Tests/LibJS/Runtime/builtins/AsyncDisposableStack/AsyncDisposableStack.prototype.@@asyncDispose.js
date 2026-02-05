test("length is 0", () => {
    expect(AsyncDisposableStack.prototype[Symbol.asyncDispose]).toHaveLength(0);
});

test("is the same as disposeAsync", () => {
    expect(AsyncDisposableStack.prototype[Symbol.asyncDispose]).toBe(AsyncDisposableStack.prototype.disposeAsync);
});
