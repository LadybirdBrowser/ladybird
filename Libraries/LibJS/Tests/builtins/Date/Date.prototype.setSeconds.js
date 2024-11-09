test("basic functionality", () => {
    let d = new Date(2000, 2, 1);

    d.setSeconds(50);
    expect(d.getSeconds()).toBe(50);

    d.setSeconds(50, 600);
    expect(d.getSeconds()).toBe(50);
    expect(d.getMilliseconds()).toBe(600);

    d.setSeconds("");
    expect(d.getSeconds()).toBe(0);

    d.setSeconds("a");
    expect(d.getSeconds()).toBe(NaN);
});

test("invalid date", () => {
    let date = new Date(NaN);
    expect(date.setSeconds(2)).toBeNaN();
    expect(date.getSeconds()).toBeNaN();
});
