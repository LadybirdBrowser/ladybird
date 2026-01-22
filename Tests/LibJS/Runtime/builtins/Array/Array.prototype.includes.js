test("length is 1", () => {
    expect(Array.prototype.includes).toHaveLength(1);
});

test("basic functionality", () => {
    var array = ["hello", "friends", 1, 2, false];

    expect([].includes()).toBeFalse();
    expect([undefined].includes()).toBeTrue();
    expect(array.includes("hello")).toBeTrue();
    expect(array.includes(1)).toBeTrue();
    expect(array.includes(1, -3)).toBeTrue();
    expect(array.includes("serenity")).toBeFalse();
    expect(array.includes(false, -1)).toBeTrue();
    expect(array.includes(2, -1)).toBeFalse();
    expect(array.includes(2, -100)).toBeTrue();
    expect(array.includes("friends", 100)).toBeFalse();
});

test("is unscopable", () => {
    expect(Array.prototype[Symbol.unscopables].includes).toBeTrue();
    const array = [];
    with (array) {
        expect(() => {
            includes;
        }).toThrowWithMessage(ReferenceError, "'includes' is not defined");
    }
});
