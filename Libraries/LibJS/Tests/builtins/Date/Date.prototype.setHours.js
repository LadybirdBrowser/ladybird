test("basic functionality", () => {
    let d = new Date(2000, 11, 15);

    d.setHours(2);
    expect(d.getHours()).toBe(2);

    d.setHours(2, 30);
    expect(d.getHours()).toBe(2);
    expect(d.getMinutes()).toBe(30);

    d.setHours(2, 30, 50);
    expect(d.getHours()).toBe(2);
    expect(d.getMinutes()).toBe(30);
    expect(d.getSeconds()).toBe(50);

    d.setHours(2, 30, 50, 600);
    expect(d.getHours()).toBe(2);
    expect(d.getMinutes()).toBe(30);
    expect(d.getSeconds()).toBe(50);
    expect(d.getMilliseconds()).toBe(600);

    d.setHours("");
    expect(d.getHours()).toBe(0);

    d.setHours("a");
    expect(d.getHours()).toBe(NaN);
});

test("invalid date", () => {
    let date = new Date(NaN);
    expect(date.setHours(2)).toBeNaN();
    expect(date.getHours()).toBeNaN();
});
