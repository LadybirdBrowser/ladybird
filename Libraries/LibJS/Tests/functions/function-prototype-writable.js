test("a function's prototype property should be writable", () => {
    function x() {}
    var desc = Object.getOwnPropertyDescriptor(x, "prototype");
    expect(desc.writable).toBe(true);
    expect(desc.enumerable).toBe(false);
    expect(desc.configurable).toBe(false);

    x.prototype = 1;
    expect(x.prototype).toBe(1);
});
