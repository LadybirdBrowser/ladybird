test("length is 1", () => {
    expect(Array.prototype.indexOf).toHaveLength(1);
});

test("basic functionality", () => {
    var array = ["hello", "friends", 1, 2, false];

    expect(array.indexOf("hello")).toBe(0);
    expect(array.indexOf("friends")).toBe(1);
    expect(array.indexOf(false)).toBe(4);
    expect(array.indexOf(false, 2)).toBe(4);
    expect(array.indexOf(false, -2)).toBe(4);
    expect(array.indexOf(1)).toBe(2);
    expect(array.indexOf(1, 1000)).toBe(-1);
    expect(array.indexOf(1, -1000)).toBe(2);
    expect(array.indexOf("serenity")).toBe(-1);
    expect(array.indexOf(false, -1)).toBe(4);
    expect(array.indexOf(2, -1)).toBe(-1);
    expect(array.indexOf(2, -2)).toBe(3);
    expect([].indexOf("serenity")).toBe(-1);
    expect([].indexOf("serenity", 10)).toBe(-1);
    expect([].indexOf("serenity", -10)).toBe(-1);
    expect([].indexOf()).toBe(-1);
    expect([undefined].indexOf()).toBe(0);
});
