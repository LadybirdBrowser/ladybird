test("JSON.isRawJSON basic functionality", () => {
    const values = [1, 1.1, null, false, true, "123"];

    for (const value of values) {
        expect(JSON.isRawJSON(value)).toBeFalse();
        expect(JSON.isRawJSON(JSON.rawJSON(value))).toBeTrue();
    }

    expect(JSON.isRawJSON(undefined)).toBeFalse();
    expect(JSON.isRawJSON(Symbol("123"))).toBeFalse();
    expect(JSON.isRawJSON([])).toBeFalse();
    expect(JSON.isRawJSON({})).toBeFalse();
    expect(JSON.isRawJSON({ rawJSON: "123" })).toBeFalse();
});
