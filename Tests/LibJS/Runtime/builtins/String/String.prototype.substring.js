test("basic functionality", () => {
    expect(String.prototype.substring).toHaveLength(2);

    expect("hello friends".substring()).toBe("hello friends");
    expect("hello friends".substring(1)).toBe("ello friends");
    expect("hello friends".substring(0, 5)).toBe("hello");
    expect("hello friends".substring(13, 6)).toBe("friends");
    expect("hello friends".substring("", 5)).toBe("hello");
    expect("hello friends".substring(3, 3)).toBe("");
    expect("hello friends".substring(-1, 13)).toBe("hello friends");
    expect("hello friends".substring(0, 50)).toBe("hello friends");
    expect("hello friends".substring(0, "5")).toBe("hello");
    expect("hello friends".substring("6", "13")).toBe("friends");
});

test("UTF-16", () => {
    var s = "😀";
    expect(s).toHaveLength(2);
    expect(s.substring()).toBe("😀");
    expect(s.substring(0)).toBe("😀");
    expect(s.substring(0, 2)).toBe("😀");
    expect(s.substring(0, 1)).toBe("\ud83d");
    expect(s.substring(1, 2)).toBe("\ude00");
    expect(s.substring(2, 2)).toBe("");
});
