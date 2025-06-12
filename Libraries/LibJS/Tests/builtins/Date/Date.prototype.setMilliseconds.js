test("basic functionality", () => {
    let d = new Date(2000, 2, 1);

    d.setMilliseconds(600);
    expect(d.getMilliseconds()).toBe(600);

    d.setMilliseconds("");
    expect(d.getMilliseconds()).toBe(0);

    d.setMilliseconds("a");
    expect(d.getMilliseconds()).toBe(NaN);
});

test("invalid date", () => {
    let date = new Date(NaN);
    expect(date.setMilliseconds(2)).toBeNaN();
    expect(date.getMilliseconds()).toBeNaN();
});
