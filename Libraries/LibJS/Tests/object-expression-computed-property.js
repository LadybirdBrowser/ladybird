test("Issue #3712, negative/non-int computed property in object expression", () => {
    const o = {
        [1.23]: "foo",
        [-1]: "foo",
        [NaN]: "foo",
        [Infinity]: "foo",
    };
    expect(o[1.23]).toBe("foo");
    expect(o[-1]).toBe("foo");
    expect(o[NaN]).toBe("foo");
    expect(o[Infinity]).toBe("foo");
});
