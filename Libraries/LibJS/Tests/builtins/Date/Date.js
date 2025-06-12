test("basic functionality", () => {
    expect(Date).toHaveLength(7);
    expect(Date.name === "Date");
    expect(Date.prototype).not.toHaveProperty("length");
});

test("string constructor", () => {
    // The string constructor is the same as calling the timestamp constructor with the result of Date.parse(arguments).
    // Since that has exhaustive tests in Date.parse.js, just do some light smoke testing here.
    expect(new Date("2017-09-07T21:08:59.001Z").toISOString()).toBe("2017-09-07T21:08:59.001Z");
});

test("timestamp constructor", () => {
    // The timestamp constructor takes a timestamp in milliseconds since the start of the epoch, in UTC.

    // 50 days and 1234 milliseconds after the start of the epoch.
    let timestamp = 50 * 24 * 60 * 60 * 1000 + 1234;

    let date = new Date(timestamp);
    expect(date.getTime()).toBe(timestamp); // getTime() returns the timestamp in UTC.
    expect(date.getUTCMilliseconds()).toBe(234);
    expect(date.getUTCSeconds()).toBe(1);
    expect(date.getUTCFullYear()).toBe(1970);
    expect(date.getUTCMonth()).toBe(1); // Feb

    date = new Date(NaN);
    expect(date.getTime()).toBe(NaN);
    date = new Date(undefined);
    expect(date.getTime()).toBe(NaN);
    date = new Date("");
    expect(date.getTime()).toBe(NaN);
});

test("tuple constructor", () => {
    // The tuple constructor takes a date in local time.
    expect(new Date(2019, 11).getFullYear()).toBe(2019);
    expect(new Date(2019, 11).getMonth()).toBe(11);
    expect(new Date(2019, 11).getDate()).toBe(1); // getDay() returns day of week, getDate() returns day in month
    expect(new Date(2019, 11).getHours()).toBe(0);
    expect(new Date(2019, 11).getMinutes()).toBe(0);
    expect(new Date(2019, 11).getSeconds()).toBe(0);
    expect(new Date(2019, 11).getMilliseconds()).toBe(0);
    expect(new Date(2019, 11).getDay()).toBe(0);

    let date = new Date(2019, 11, 15, 9, 16, 14, 123); // Note: Month is 0-based.
    expect(date.getFullYear()).toBe(2019);
    expect(date.getMonth()).toBe(11);
    expect(date.getDate()).toBe(15);
    expect(date.getHours()).toBe(9);
    expect(date.getMinutes()).toBe(16);
    expect(date.getSeconds()).toBe(14);
    expect(date.getMilliseconds()).toBe(123);
    expect(date.getDay()).toBe(0);

    // getTime() returns a time stamp in UTC, but we can at least check it's in the right interval, which will be true independent of the local timezone if the range is big enough.
    let timestamp_lower_bound = 1575072000000; // 2019-12-01T00:00:00Z
    let timestamp_upper_bound = 1577750400000; // 2019-12-31T00:00:00Z
    expect(date.getTime()).toBeGreaterThan(timestamp_lower_bound);
    expect(date.getTime()).toBeLessThan(timestamp_upper_bound);

    date = new Date(NaN, 11, 15, 9, 16, 14, 123);
    expect(date.getTime()).toBe(NaN);
    date = new Date(2021, 11, 15, 9, 16, 14, undefined);
    expect(date.getTime()).toBe(NaN);
});

test("tuple constructor overflow", () => {
    let date = new Date(2019, 13, 33, 30, 70, 80, 2345);
    expect(date.getFullYear()).toBe(2020);
    expect(date.getMonth()).toBe(2);
    expect(date.getDate()).toBe(5);
    expect(date.getHours()).toBe(7);
    expect(date.getMinutes()).toBe(11);
    expect(date.getSeconds()).toBe(22);
    expect(date.getMilliseconds()).toBe(345);
    expect(date.getDay()).toBe(4);

    date = new Date(2019, -13, -33, -30, -70, -80, -2345);
    expect(date.getFullYear()).toBe(2017);
    expect(date.getMonth()).toBe(9);
    expect(date.getDate()).toBe(26);
    expect(date.getHours()).toBe(16);
    expect(date.getMinutes()).toBe(48);
    expect(date.getSeconds()).toBe(37);
    expect(date.getMilliseconds()).toBe(655);
    expect(date.getDay()).toBe(4);
});
