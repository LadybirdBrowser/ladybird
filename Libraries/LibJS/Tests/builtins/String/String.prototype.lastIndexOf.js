test("basic functionality", () => {
    expect(String.prototype.lastIndexOf).toHaveLength(1);

    expect("hello friends".lastIndexOf()).toBe(-1);
    expect("hello friends".lastIndexOf("e")).toBe(9);
    expect("hello friends".lastIndexOf("e", -7)).toBe(-1);
    expect("hello friends".lastIndexOf("e", 100)).toBe(9);
    expect("hello friends".lastIndexOf("")).toBe(13);
    expect("hello friends".lastIndexOf("Z")).toBe(-1);
    expect("hello friends".lastIndexOf("serenity")).toBe(-1);
    expect("hello friends".lastIndexOf("", 4)).toBe(4);
    expect("hello serenity friends".lastIndexOf("serenity")).toBe(6);
    expect("hello serenity friends serenity".lastIndexOf("serenity")).toBe(23);
    expect("hello serenity friends serenity".lastIndexOf("serenity", 14)).toBe(6);
    expect("".lastIndexOf("")).toBe(0);
    expect("".lastIndexOf("", 1)).toBe(0);
    expect("".lastIndexOf("", -1)).toBe(0);
    expect("hello friends serenity".lastIndexOf("h", 10)).toBe(0);
    expect("hello friends serenity".lastIndexOf("l", 4)).toBe(3);
    expect("hello friends serenity".lastIndexOf("s", 13)).toBe(12);
    expect("hello".lastIndexOf("serenity")).toBe(-1);
});

test("UTF-16", () => {
    var s = "😀";
    expect(s.lastIndexOf("😀")).toBe(0);
    expect(s.lastIndexOf("\ud83d")).toBe(0);
    expect(s.lastIndexOf("\ude00")).toBe(1);
    expect(s.lastIndexOf("a")).toBe(-1);
});
