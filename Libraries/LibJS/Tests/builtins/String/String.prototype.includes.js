test("basic functionality", () => {
    expect(String.prototype.includes).toHaveLength(1);

    expect("hello friends".includes("hello")).toBeTrue();
    expect("hello friends".includes("hello", 100)).toBeFalse();
    expect("hello friends".includes("hello", -10)).toBeTrue();
    expect("hello friends".includes("friends", 6)).toBeTrue();
    expect("hello friends".includes("hello", 6)).toBeFalse();
    expect("hello friends false".includes(false)).toBeTrue();
    expect("hello 10 friends".includes(10)).toBeTrue();
    expect("hello friends undefined".includes()).toBeTrue();
});

test("UTF-16", () => {
    var s = "😀";
    expect(s.includes("😀")).toBeTrue();
    expect(s.includes("\ud83d")).toBeTrue();
    expect(s.includes("\ude00")).toBeTrue();
    expect(s.includes("a")).toBeFalse();
});
