test("basic functionality", () => {
    expect(String.prototype.slice).toHaveLength(2);

    expect("hello friends".slice()).toBe("hello friends");
    expect("hello friends".slice(1)).toBe("ello friends");
    expect("hello friends".slice(0, 5)).toBe("hello");
    expect("hello friends".slice(13, 6)).toBe("");
    expect("hello friends".slice("", 5)).toBe("hello");
    expect("hello friends".slice(3, 3)).toBe("");
    expect("hello friends".slice(-1, 13)).toBe("s");
    expect("hello friends".slice(0, 50)).toBe("hello friends");
    expect("hello friends".slice(0, "5")).toBe("hello");
    expect("hello friends".slice("6", "13")).toBe("friends");
    expect("hello friends".slice(-7)).toBe("friends");
    expect("hello friends".slice(1000)).toBe("");
    expect("hello friends".slice(-1000)).toBe("hello friends");
});

test("UTF-16", () => {
    var s = "😀";
    expect(s).toHaveLength(2);
    expect(s.slice()).toBe("😀");
    expect(s.slice(0)).toBe("😀");
    expect(s.slice(1)).toBe("\ude00");
    expect(s.slice(0, 1)).toBe("\ud83d");
    expect(s.slice(0, 2)).toBe("😀");
});
