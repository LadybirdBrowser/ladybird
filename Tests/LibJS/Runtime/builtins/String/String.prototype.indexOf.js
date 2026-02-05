test("basic functionality", () => {
    expect(String.prototype.indexOf).toHaveLength(1);

    var s = "hello friends";

    expect(s.indexOf("friends")).toBe(6);
    expect(s.indexOf("enemies")).toBe(-1);

    expect(s.indexOf("friends", 0)).toBe(6);
    expect(s.indexOf("enemies", 0)).toBe(-1);

    expect(s.indexOf("friends", 4)).toBe(6);
    expect(s.indexOf("friends", 6)).toBe(6);
    expect(s.indexOf("friends", 7)).toBe(-1);
    expect(s.indexOf("friends", 8)).toBe(-1);

    expect(s.indexOf("enemies", 2)).toBe(-1);
    expect(s.indexOf("enemies", 7)).toBe(-1);

    expect(s.indexOf("e")).toBe(1);
    expect(s.indexOf("e", 0)).toBe(1);
    expect(s.indexOf("e", 2)).toBe(9);
});

test("UTF-16", () => {
    var s = "😀";
    expect(s.indexOf("😀")).toBe(0);
    expect(s.indexOf("\ud83d")).toBe(0);
    expect(s.indexOf("\ude00")).toBe(1);
    expect(s.indexOf("a")).toBe(-1);
});
