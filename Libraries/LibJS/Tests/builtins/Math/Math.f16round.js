test("basic functionality", () => {
    expect(Math.f16round).toHaveLength(1);

    expect(Math.f16round(5.5)).toBe(5.5);
    expect(Math.f16round(5.05)).toBe(5.05078125);
    expect(Math.f16round(5)).toBe(5);
    expect(Math.f16round(-5.05)).toBe(-5.05078125);
});
