test("basic update expression", () => {
    const o = {};
    o.f = 1;

    expect(o.f++).toBe(1);
    expect(++o.f).toBe(3);
    expect(++o.missing).toBeNaN();
});
