test("basic functionality", () => {
    expect(Map.prototype.get).toHaveLength(1);

    const map = new Map([
        ["a", 0],
        ["b", 1],
        ["c", 2],
    ]);
    expect(map.get("a")).toBe(0);
    expect(map.get("d")).toBe(undefined);
});

test("NaN differentiation", () => {
    const map = new Map();
    map.set(NaN, "a");

    expect(map.get(0 / 0)).toBe("a");
    expect(map.get(0 * Infinity)).toBe("a");
    expect(map.get(Infinity - Infinity)).toBe("a");
});
