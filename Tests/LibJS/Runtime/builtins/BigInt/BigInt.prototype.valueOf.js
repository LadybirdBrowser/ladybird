test("basic functionality", () => {
    expect(BigInt.prototype.valueOf).toHaveLength(0);
    expect(typeof BigInt(123).valueOf()).toBe("bigint");
    expect(typeof Object(123n).valueOf()).toBe("bigint");
});

test("calling with non-BigInt |this|", () => {
    expect(() => {
        BigInt.prototype.valueOf.call("foo");
    }).toThrowWithMessage(TypeError, "Not an object of type BigInt");
});
