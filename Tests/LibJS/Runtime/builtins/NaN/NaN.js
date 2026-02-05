test("basic functionality", () => {
    const nan = undefined + 1;

    expect(nan + "").toBe("NaN");
    expect(NaN + "").toBe("NaN");
    expect(nan !== nan).toBeTrue();
    expect(NaN !== NaN).toBeTrue();
    expect(nan).toBeNaN();
    expect(NaN).toBeNaN();
    expect(0).not.toBeNaN();
    expect(!!nan).toBeFalse();
    expect(!!NaN).toBeFalse();
});
