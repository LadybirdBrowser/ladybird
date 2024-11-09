test("basic functionality", () => {
    let d = new Date();
    expect(d.getDate()).toBe(d.getDate());
    expect(d.getDate()).not.toBeNaN();
    expect(d.getDate()).toBeGreaterThanOrEqual(1);
    expect(d.getDate()).toBeLessThanOrEqual(31);
    expect(new Date(NaN).getDate()).toBe(NaN);
});
