test("basic arguments.callee", () => {
    var foo = function () {
        return arguments.callee;
    };
    expect(foo()).toBe(foo);
});
