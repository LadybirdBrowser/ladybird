test("basic functionality", () => {
    expect(BigInt.prototype.toString).toHaveLength(0);
    expect(BigInt(123).toString()).toBe("123");
});

test("calling with non-BigInt |this|", () => {
    expect(() => {
        BigInt.prototype.toString.call("foo");
    }).toThrowWithMessage(TypeError, "Not an object of type BigInt");
});
