test("basic functionality", () => {
    const o = {
        [Symbol.toPrimitive]: hint => {
            lastHint = hint;
        },
    };
    let lastHint;

    // Calls ToPrimitive abstract operation with 'string' hint
    String(o);
    expect(lastHint).toBe("string");

    // Calls ToPrimitive abstract operation with 'number' hint
    +o;
    expect(lastHint).toBe("number");

    // Calls ToPrimitive abstract operation with 'default' hint
    "" + o;
    expect(lastHint).toBe("default");
});
