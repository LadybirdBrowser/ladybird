test("basic functionality", () => {
    var d = new Date();
    expect(d.getTime()).toBe(d.getTime());
    expect(d.getTime()).not.toBeNaN();
    expect(d.getTime()).toBeGreaterThanOrEqual(1580000000000);
    expect(new Date(NaN).getTime()).toBe(NaN);
});
