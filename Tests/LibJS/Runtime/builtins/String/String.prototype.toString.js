test("basic functionality", () => {
    expect(String.prototype.toString).toHaveLength(0);

    expect("".toString()).toBe("");
    expect("hello friends".toString()).toBe("hello friends");
});

test("UTF-16", () => {
    expect("😀".toString()).toBe("😀");
    expect("😀😀😀".toString()).toBe("😀😀😀");
});
