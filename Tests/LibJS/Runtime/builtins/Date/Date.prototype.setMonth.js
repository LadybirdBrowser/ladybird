test("no arguments", () => {
    let date = new Date(2021, 0, 1);
    date.setMonth();
    expect(date.getTime()).toBe(NaN);
});

test("NaN or undefined as only argument", () => {
    let date = new Date(2021, 0, 1);
    date.setMonth(NaN);
    expect(date.getTime()).toBe(NaN);

    date = new Date(2021, 0, 1);
    date.setMonth(undefined);
    expect(date.getTime()).toBe(NaN);

    date = new Date(2021, 0, 1);
    date.setMonth("a");
    expect(date.getTime()).toBe(NaN);
});

test("Only month as argument", () => {
    let date = new Date(2021, 0, 1);

    date.setMonth(7);
    expect(date.getFullYear()).toBe(2021);
    expect(date.getMonth()).toBe(7);
    expect(date.getDate()).toBe(1);
    expect(date.getHours()).toBe(0);
    expect(date.getMinutes()).toBe(0);
    expect(date.getSeconds()).toBe(0);
    expect(date.getMilliseconds()).toBe(0);
});

test("Month and date as arguments", () => {
    let date = new Date(2021, 0, 1);

    date.setMonth(7, 3);
    expect(date.getFullYear()).toBe(2021);
    expect(date.getMonth()).toBe(7);
    expect(date.getDate()).toBe(3);
    expect(date.getHours()).toBe(0);
    expect(date.getMinutes()).toBe(0);
    expect(date.getSeconds()).toBe(0);
    expect(date.getMilliseconds()).toBe(0);
});

test("NaN or undefined in any arguments", () => {
    let date = new Date(2021, 0, 1);
    date.setMonth(NaN, 3);
    expect(date.getTime()).toBe(NaN);

    date = new Date(2021, 0, 1);
    date.setMonth(2021, NaN);
    expect(date.getTime()).toBe(NaN);

    date = new Date(2021, 0, 1);
    date.setMonth(undefined, 3);
    expect(date.getTime()).toBe(NaN);

    date = new Date(2021, 0, 1);
    date.setMonth(2021, undefined);
    expect(date.getTime()).toBe(NaN);
});

test("invalid date", () => {
    let date = new Date(NaN);
    expect(date.setMonth(2)).toBeNaN();
    expect(date.getMonth()).toBeNaN();
});
