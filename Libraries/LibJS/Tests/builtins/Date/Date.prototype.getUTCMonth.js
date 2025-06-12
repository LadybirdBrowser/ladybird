test("basic functionality", () => {
    var d = new Date();
    expect(d.getUTCMonth()).toBe(d.getUTCMonth());
    expect(d.getUTCMonth()).not.toBeNaN();
    expect(d.getUTCMonth()).toBeGreaterThanOrEqual(0);
    expect(d.getUTCMonth()).toBeLessThanOrEqual(11);

    expect(new Date(Date.UTC(2020, 11)).getUTCMonth()).toBe(11);
    expect(new Date(NaN).getUTCMonth()).toBe(NaN);
});

test("leap years", () => {
    expect(new Date(Date.UTC(2019, 1, 29)).getUTCDate()).toBe(1);
    expect(new Date(Date.UTC(2019, 1, 29)).getUTCMonth()).toBe(2);
    expect(new Date(Date.UTC(2100, 1, 29)).getUTCDate()).toBe(1);
    expect(new Date(Date.UTC(2100, 1, 29)).getUTCMonth()).toBe(2);

    expect(new Date(Date.UTC(2000, 1, 29)).getUTCDate()).toBe(29);
    expect(new Date(Date.UTC(2000, 1, 29)).getUTCMonth()).toBe(1);
    expect(new Date(Date.UTC(2020, 1, 29)).getUTCDate()).toBe(29);
    expect(new Date(Date.UTC(2020, 1, 29)).getUTCMonth()).toBe(1);

    expect(new Date(Date.UTC(2019, 2, 1)).getUTCDate()).toBe(1);
    expect(new Date(Date.UTC(2019, 2, 1)).getUTCMonth()).toBe(2);
    expect(new Date(Date.UTC(2020, 2, 1)).getUTCDate()).toBe(1);
    expect(new Date(Date.UTC(2020, 2, 1)).getUTCMonth()).toBe(2);
});
