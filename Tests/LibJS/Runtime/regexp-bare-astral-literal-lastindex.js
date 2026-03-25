test("bare astral literals do not snap a global unicode search back to the pair start", () => {
    let matcher = /😀/gu;
    matcher.lastIndex = 1;
    expect(matcher.exec("😀")).toBeNull();
    expect(matcher.lastIndex).toBe(0);

    matcher = /(?:😀)/gu;
    matcher.lastIndex = 1;
    let match = matcher.exec("😀");
    expect(match).not.toBeNull();
    expect(match[0]).toBe("😀");
    expect(match.index).toBe(0);
    expect(matcher.lastIndex).toBe(2);
});
