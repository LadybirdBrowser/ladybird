test("invariants", () => {
    expect(Atomics.pause).toHaveLength(0);
});

test("error cases", () => {
    expect(() => {
        Atomics.pause({});
    }).toThrow(TypeError);

    expect(() => {
        Atomics.pause("not an integer");
    }).toThrow(TypeError);

    expect(() => {
        Atomics.pause("0");
    }).toThrow(TypeError);
});

test("basic functionality", () => {
    expect(Atomics.pause()).toBeUndefined();
    expect(Atomics.pause(0)).toBeUndefined();
    expect(Atomics.pause(1)).toBeUndefined();
    expect(Atomics.pause(-1)).toBeUndefined();
});
