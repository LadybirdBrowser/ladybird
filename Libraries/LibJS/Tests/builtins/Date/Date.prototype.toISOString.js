test("basic functionality", () => {
    expect(new Date(1597955034555).toISOString()).toBe("2020-08-20T20:23:54.555Z");
    expect(new Date(Date.UTC(22020)).toISOString()).toBe("+022020-01-01T00:00:00.000Z");
    expect(new Date(Date.UTC(1950)).toISOString()).toBe("1950-01-01T00:00:00.000Z");
    expect(new Date(Date.UTC(1800)).toISOString()).toBe("1800-01-01T00:00:00.000Z");
    expect(new Date(Date.UTC(-100)).toISOString()).toBe("-000100-01-01T00:00:00.000Z");

    expect(() => {
        new Date(NaN).toISOString();
    }).toThrowWithMessage(RangeError, "Invalid time value");
});
