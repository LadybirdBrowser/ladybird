test("object with custom toString", () => {
    const o = { toString: () => "foo" };
    expect(o + "bar").toBe("foobar");
    expect([o, "bar"].toString()).toBe("foo,bar");
});

test("object with uncallable toString and custom valueOf", () => {
    const o = { toString: undefined, valueOf: () => "foo" };
    expect(o + "bar").toBe("foobar");
    expect([o, "bar"].toString()).toBe("foo,bar");
});

test("object with custom valueOf", () => {
    const o = { valueOf: () => 42 };
    expect(Number(o)).toBe(42);
    expect(o + 1).toBe(43);
});

test("object with uncallable valueOf and custom toString", () => {
    const o = { valueOf: undefined, toString: () => "42" };
    expect(Number(o)).toBe(42);
    expect(o + 1).toBe("421");
});
