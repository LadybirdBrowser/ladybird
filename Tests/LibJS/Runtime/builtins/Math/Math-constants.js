test("basic functionality", () => {
    expect(Math.E).toBeCloseTo(2.718281);
    expect(Math.LN2).toBeCloseTo(0.693147);
    expect(Math.LN10).toBeCloseTo(2.302585);
    expect(Math.LOG2E).toBeCloseTo(1.442695);
    expect(Math.LOG10E).toBeCloseTo(0.434294);
    expect(Math.PI).toBeCloseTo(3.1415926);
    expect(Math.SQRT1_2).toBeCloseTo(0.707106);
    expect(Math.SQRT2).toBeCloseTo(1.414213);
});
