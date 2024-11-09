test("basic functionality", () => {
    expect(~0).toBe(-1);
    expect(~1).toBe(-2);
    expect(~2).toBe(-3);
    expect(~3).toBe(-4);
    expect(~4).toBe(-5);
    expect(~5).toBe(-6);
    expect(~-1).toBe(0);
    expect(~42).toBe(-43);
    expect(~9999).toBe(-10000);
});

test("non-numeric values", () => {
    expect(~"42").toBe(-43);
    expect(~"foo").toBe(-1);
    expect(~[]).toBe(-1);
    expect(~{}).toBe(-1);
    expect(~undefined).toBe(-1);
    expect(~null).toBe(-1);
    expect(~NaN).toBe(-1);
    expect(~Infinity).toBe(-1);
    expect(~-Infinity).toBe(-1);
});
