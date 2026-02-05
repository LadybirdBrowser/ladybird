test("basic functionality", () => {
    expect(String.prototype.concat).toHaveLength(1);

    expect("".concat(1)).toBe("1");
    expect("".concat(3, 2, 1)).toBe("321");
    expect("hello".concat(" ", "friends")).toBe("hello friends");
    expect("".concat(null)).toBe("null");
    expect("".concat(false)).toBe("false");
    expect("".concat(true)).toBe("true");
    expect("".concat([])).toBe("");
    expect("".concat([1, 2, 3, "hello"])).toBe("1,2,3,hello");
    expect("".concat(true, [])).toBe("true");
    expect("".concat(true, false)).toBe("truefalse");
    expect("".concat({})).toBe("[object Object]");
    expect("".concat(1, {})).toBe("1[object Object]");
    expect("".concat(1, {}, false)).toBe("1[object Object]false");
});

test("UTF-16", () => {
    expect("😀".concat()).toBe("😀");
    expect("😀".concat("a")).toBe("😀a");
    expect("😀".concat("a", 4)).toBe("😀a4");
    expect("😀".concat("a", "😀")).toBe("😀a😀");
});
