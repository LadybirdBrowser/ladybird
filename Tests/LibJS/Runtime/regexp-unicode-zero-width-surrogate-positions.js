test("unicode zero-width patterns can match at surrogate interior positions", () => {
    let global_matcher = /\B/gu;
    global_matcher.lastIndex = 2;

    let match = global_matcher.exec("A😀");
    expect(match).not.toBeNull();
    expect(match[0]).toBe("");
    expect(match.index).toBe(2);
    expect(global_matcher.lastIndex).toBe(2);

    match = /(?!😀)/u.exec("😀");
    expect(match).not.toBeNull();
    expect(match[0]).toBe("");
    expect(match.index).toBe(1);
});
