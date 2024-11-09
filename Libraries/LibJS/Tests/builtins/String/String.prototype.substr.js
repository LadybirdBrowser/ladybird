test("basic functionality", () => {
    expect(String.prototype.substr).toHaveLength(2);

    expect("".substr(1)).toBe("");
    expect("".substr()).toBe("");
    expect("".substr(-1)).toBe("");
    expect("a".substr(-1)).toBe("a");
    expect("a".substr(-2)).toBe("a");
    expect("a".substr(-3)).toBe("a");
    expect("hello friends".substr()).toBe("hello friends");
    expect("hello friends".substr(1)).toBe("ello friends");
    expect("hello friends".substr(0, 5)).toBe("hello");
    expect("hello friends".substr(5, 6)).toBe(" frien");
    expect("hello friends".substr("", 5)).toBe("hello");
    expect("hello friends".substr(3, 3)).toBe("lo ");
    expect("hello friends".substr(-1, 13)).toBe("s");
    expect("hello friends".substr(0, 50)).toBe("hello friends");
    expect("hello friends".substr(0, "5")).toBe("hello");
    expect("hello friends".substr("2", "2")).toBe("ll");
    expect("hello friends".substr(-7)).toBe("friends");
    expect("hello friends".substr(-3, -5)).toBe("");
});

test("Non numeric values", () => {
    expect("a".substr(0, Infinity)).toBe("a");
    expect("a".substr(0, -Infinity)).toBe("");
    expect("abc".substr(0, Infinity)).toBe("abc");
    expect("abc".substr(0, -Infinity)).toBe("");
    expect("abc".substr(Infinity, Infinity)).toBe("");
    expect("abc".substr(Infinity)).toBe("");
    expect("abc".substr(-Infinity)).toBe("abc");
    expect("abc".substr(-Infinity, 1)).toBe("a");
    expect("abc".substr(-Infinity, Infinity)).toBe("abc");

    expect("abc".substr(NaN)).toBe("abc");
    expect("abc".substr(NaN, NaN)).toBe("");
    expect("abc".substr(0, NaN)).toBe("");
    expect("abc".substr(NaN, Infinity)).toBe("abc");
    expect("abc".substr(NaN, -Infinity)).toBe("");
});

test("UTF-16", () => {
    var s = "😀";
    expect(s).toHaveLength(2);
    expect(s.substr()).toBe("😀");
    expect(s.substr(0)).toBe("😀");
    expect(s.substr(0, 2)).toBe("😀");
    expect(s.substr(0, 1)).toBe("\ud83d");
    expect(s.substr(1, 1)).toBe("\ude00");
    expect(s.substr(2, 1)).toBe("");
});
