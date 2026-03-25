test("greedy lookbehind backtracking makes progress at end of input", () => {
    expect(/(?<=a.?)/.exec("b")).toBeNull();

    let match = /(?<=a.?)/.exec("ab");
    expect(match).not.toBeNull();
    expect(match[0]).toBe("");
    expect(match.index).toBe(1);
});
