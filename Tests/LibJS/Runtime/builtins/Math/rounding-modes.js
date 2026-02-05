test("basic rounding", () => {
    expect(Math.round(1.25)).toBe(1);
    expect(Math.round(-1.25)).toBe(-1);
    expect(Math.round(1.5)).toBe(2);
    expect(Math.round(-1.5)).toBe(-1);
    expect(Math.round(1.75)).toBe(2);
    expect(Math.round(-1.75)).toBe(-2);
    expect(Math.round(1)).toBe(1);
    expect(Math.round(-1)).toBe(-1);
    expect(Math.round(4294967296.5)).toBe(4294967297);
    expect(Math.round(-4294967296.5)).toBe(-4294967296);
    expect(Math.round(4294967297)).toBe(4294967297);
    expect(Math.round(-4294967297)).toBe(-4294967297);
});
test("basic floor", () => {
    expect(Math.floor(1.25)).toBe(1);
    expect(Math.floor(-1.25)).toBe(-2);
    expect(Math.floor(1.5)).toBe(1);
    expect(Math.floor(-1.5)).toBe(-2);
    expect(Math.floor(1.75)).toBe(1);
    expect(Math.floor(-1.75)).toBe(-2);
    expect(Math.floor(1)).toBe(1);
    expect(Math.floor(-1)).toBe(-1);
    expect(Math.floor(4294967296.5)).toBe(4294967296);
    expect(Math.floor(-4294967296.5)).toBe(-4294967297);
    expect(Math.floor(4294967297)).toBe(4294967297);
    expect(Math.floor(-4294967297)).toBe(-4294967297);
});
