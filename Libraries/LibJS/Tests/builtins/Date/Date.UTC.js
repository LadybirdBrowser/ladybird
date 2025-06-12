test("length is 7", () => {
    expect(Date.UTC).toHaveLength(7);
});

test("basic functionality", () => {
    expect(Date.UTC(2020)).toBe(1577836800000);
    expect(Date.UTC(2000, 10)).toBe(973036800000);
    expect(Date.UTC(1980, 5, 30)).toBe(331171200000);
    expect(Date.UTC(1980, 5, 30, 13)).toBe(331218000000);
    expect(Date.UTC(1970, 5, 30, 13, 30)).toBe(15600600000);
    expect(Date.UTC(1970, 0, 1, 0, 0, 59)).toBe(59000);
    expect(Date.UTC(1970, 0, 1, 0, 0, 0, 999)).toBe(999);

    expect(Date.UTC(1969, 11, 31, 23, 59, 59, 817)).toBe(-183);

    expect(Date.UTC(1799, 0)).toBe(-5396198400000);
    expect(Date.UTC(1800, 0)).toBe(-5364662400000);
    expect(Date.UTC(1801, 0)).toBe(-5333126400000);
    expect(Date.UTC(1802, 0)).toBe(-5301590400000);
    expect(Date.UTC(1803, 0)).toBe(-5270054400000);
    expect(Date.UTC(1804, 0)).toBe(-5238518400000);

    expect(Date.UTC(1999, 0)).toBe(915148800000);
    expect(Date.UTC(2000, 0)).toBe(946684800000);
    expect(Date.UTC(2001, 0)).toBe(978307200000);
    expect(Date.UTC(2002, 0)).toBe(1009843200000);
    expect(Date.UTC(2003, 0)).toBe(1041379200000);
    expect(Date.UTC(2004, 0)).toBe(1072915200000);

    expect(Date.UTC(20000, 0)).toBe(568971820800000);
});

test("leap year", () => {
    expect(Date.UTC(2020, 2, 1)).toBe(1583020800000);
});

test("out of range", () => {
    expect(Date.UTC(2020, -20)).toBe(1525132800000);
    expect(Date.UTC(2020, 20)).toBe(1630454400000);

    expect(Date.UTC(2020, 1, -10)).toBe(1579564800000);
    expect(Date.UTC(2020, 1, 40)).toBe(1583884800000);

    expect(Date.UTC(2020, 1, 15, -50)).toBe(1581544800000);
    expect(Date.UTC(2020, 1, 15, 50)).toBe(1581904800000);

    expect(Date.UTC(2020, 1, 15, 12, -123)).toBe(1581760620000);
    expect(Date.UTC(2020, 1, 15, 12, 123)).toBe(1581775380000);

    expect(Date.UTC(2020, 1, 15, 12, 30, -123)).toBe(1581769677000);
    expect(Date.UTC(2020, 1, 15, 12, 30, 123)).toBe(1581769923000);

    expect(Date.UTC(2020, 1, 15, 12, 30, 30, -2345)).toBe(1581769827655);
    expect(Date.UTC(2020, 1, 15, 12, 30, 30, 2345)).toBe(1581769832345);
});

test("special values", () => {
    [Infinity, -Infinity, NaN].forEach(value => {
        expect(Date.UTC(value)).toBeNaN();
        expect(Date.UTC(0, value)).toBeNaN();
        expect(Date.UTC(0, 0, value)).toBeNaN();
        expect(Date.UTC(0, 0, 1, value)).toBeNaN();
        expect(Date.UTC(0, 0, 1, 0, value)).toBeNaN();
        expect(Date.UTC(0, 0, 1, 0, 0, value)).toBeNaN();
        expect(Date.UTC(0, 0, 1, 0, 0, 0, value)).toBeNaN();
    });
});

test("time clip", () => {
    expect(Date.UTC(275760, 8, 13, 0, 0, 0, 0)).toBe(8.64e15);
    expect(Date.UTC(275760, 8, 13, 0, 0, 0, 1)).toBeNaN();
});

test("YearFromTime invariant holds with negative times", () => {
    // https://tc39.es/ecma262/#sec-yearfromtime: YearFromTime(t) should return
    // a value such that TimeFromYear(YearFromTime(t)) <= t.
    //
    // If this doesn't hold, then the following Date constructor will result in
    // a crash from an assertion (#21548).
    new Date(Date.UTC(-1112, 11, 31));
});
