test("basic functionality", () => {
    expect(Array.prototype.at).toHaveLength(1);

    const array = ["a", "b", "c"];
    expect(array.at(0)).toBe("a");
    expect(array.at(1)).toBe("b");
    expect(array.at(2)).toBe("c");
    expect(array.at(3)).toBeUndefined();
    expect(array.at(Infinity)).toBeUndefined();
    expect(array.at(-1)).toBe("c");
    expect(array.at(-2)).toBe("b");
    expect(array.at(-3)).toBe("a");
    expect(array.at(-4)).toBeUndefined();
    expect(array.at(-Infinity)).toBeUndefined();
});

test("is unscopable", () => {
    expect(Array.prototype[Symbol.unscopables].at).toBeTrue();
    const array = [];
    with (array) {
        expect(() => {
            at;
        }).toThrowWithMessage(ReferenceError, "'at' is not defined");
    }
});
