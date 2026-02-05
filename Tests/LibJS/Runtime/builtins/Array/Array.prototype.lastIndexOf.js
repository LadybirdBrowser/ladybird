test("length is 1", () => {
    expect(Array.prototype.lastIndexOf).toHaveLength(1);
});

test("basic functionality", () => {
    var array = [1, 2, 3, 1, "hello"];

    expect(array.lastIndexOf("hello")).toBe(4);
    expect(array.lastIndexOf("hello", 1000)).toBe(4);
    expect(array.lastIndexOf(1)).toBe(3);
    expect(array.lastIndexOf(1, -1)).toBe(3);
    expect(array.lastIndexOf(1, -2)).toBe(3);
    expect(array.lastIndexOf(2)).toBe(1);
    expect(array.lastIndexOf(2, -3)).toBe(1);
    expect(array.lastIndexOf(2, -4)).toBe(1);
    expect([].lastIndexOf("hello")).toBe(-1);
    expect([].lastIndexOf("hello", 10)).toBe(-1);
    expect([].lastIndexOf("hello", -10)).toBe(-1);
    expect([].lastIndexOf()).toBe(-1);
    expect([undefined].lastIndexOf()).toBe(0);
    expect([undefined, undefined, undefined].lastIndexOf()).toBe(2);
});
