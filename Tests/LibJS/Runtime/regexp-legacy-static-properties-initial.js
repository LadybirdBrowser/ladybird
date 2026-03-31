test("legacy static properties return empty string before any match", () => {
    expect(RegExp.$1).toBe("");
    expect(RegExp.$2).toBe("");
    expect(RegExp.$3).toBe("");
    expect(RegExp.$4).toBe("");
    expect(RegExp.$5).toBe("");
    expect(RegExp.$6).toBe("");
    expect(RegExp.$7).toBe("");
    expect(RegExp.$8).toBe("");
    expect(RegExp.$9).toBe("");
    expect(RegExp.input).toBe("");
    expect(RegExp.lastMatch).toBe("");
    expect(RegExp.lastParen).toBe("");
    expect(RegExp.leftContext).toBe("");
    expect(RegExp.rightContext).toBe("");
});
