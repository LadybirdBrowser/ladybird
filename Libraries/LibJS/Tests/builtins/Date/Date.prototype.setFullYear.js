test("no arguments", () => {
    let date = new Date(2021, 0, 1);
    date.setFullYear();
    expect(date.getTime()).toBe(NaN);
});

test("NaN or undefined as only argument", () => {
    let date = new Date(2021, 0, 1);
    date.setFullYear(NaN);
    expect(date.getTime()).toBe(NaN);

    date = new Date(2021, 0, 1);
    date.setFullYear(undefined);
    expect(date.getTime()).toBe(NaN);

    date = new Date(2021, 0, 1);
    date.setFullYear("");
    expect(date.getFullYear()).toBe(0);

    date = new Date(2021, 0, 1);
    date.setFullYear("a");
    expect(date.getTime()).toBe(NaN);
});

test("Only year as argument", () => {
    let date = new Date(2021, 0, 1);

    date.setFullYear(1992);
    expect(date.getFullYear()).toBe(1992);
    expect(date.getMonth()).toBe(0);
    expect(date.getDate()).toBe(1);
    expect(date.getHours()).toBe(0);
    expect(date.getMinutes()).toBe(0);
    expect(date.getSeconds()).toBe(0);
    expect(date.getMilliseconds()).toBe(0);
});

test("Year and month as arguments", () => {
    let date = new Date(2021, 0, 1);

    date.setFullYear(2021, 3);
    expect(date.getFullYear()).toBe(2021);
    expect(date.getMonth()).toBe(3);
    expect(date.getDate()).toBe(1);
    expect(date.getHours()).toBe(0);
    expect(date.getMinutes()).toBe(0);
    expect(date.getSeconds()).toBe(0);
    expect(date.getMilliseconds()).toBe(0);
});

test("Year, month, and day as arguments", () => {
    let date = new Date(2021, 0, 1);

    date.setFullYear(2021, 3, 16);
    expect(date.getFullYear()).toBe(2021);
    expect(date.getMonth()).toBe(3);
    expect(date.getDate()).toBe(16);
    expect(date.getHours()).toBe(0);
    expect(date.getMinutes()).toBe(0);
    expect(date.getSeconds()).toBe(0);
    expect(date.getMilliseconds()).toBe(0);
});

test("NaN or undefined in any arguments", () => {
    let date = new Date(2021, 0, 1);
    date.setFullYear(NaN, 3, 16);
    expect(date.getTime()).toBe(NaN);

    date = new Date(2021, 0, 1);
    date.setFullYear(2021, NaN, 16);
    expect(date.getTime()).toBe(NaN);

    date = new Date(2021, 0, 1);
    date.setFullYear(2021, 3, NaN);
    expect(date.getTime()).toBe(NaN);

    date = new Date(2021, 0, 1);
    date.setFullYear(undefined, 3, 16);
    expect(date.getTime()).toBe(NaN);

    date = new Date(2021, 0, 1);
    date.setFullYear(2021, undefined, 16);
    expect(date.getTime()).toBe(NaN);

    date = new Date(2021, 0, 1);
    date.setFullYear(2021, 3, undefined);
    expect(date.getTime()).toBe(NaN);

    date.setFullYear(2021, 3, 16);
    expect(date.getFullYear()).toBe(2021);
    expect(date.getMonth()).toBe(3);
    expect(date.getDate()).toBe(16);
    expect(date.getHours()).toBe(0);
    expect(date.getMinutes()).toBe(0);
    expect(date.getSeconds()).toBe(0);
    expect(date.getMilliseconds()).toBe(0);
});

test("Make Invalid Date valid again", () => {
    let date = new Date(2021, 0, 1);
    date.setFullYear(NaN, 3, 16);
    expect(date.getTime()).toBe(NaN);

    date.setFullYear(2021, 3, 16);
    expect(date.getFullYear()).toBe(2021);
    expect(date.getMonth()).toBe(3);
    expect(date.getDate()).toBe(16);
    expect(date.getHours()).toBe(0);
    expect(date.getMinutes()).toBe(0);
    expect(date.getSeconds()).toBe(0);
    expect(date.getMilliseconds()).toBe(0);
});

test("invalid date", () => {
    let date = new Date(NaN);
    date.setFullYear(2022);
    expect(date.getFullYear()).toBe(2022);
});
