test("length is 1", () => {
    expect(Map.prototype.has).toHaveLength(1);
});

test("basic functionality", () => {
    var map = new Map([
        ["a", 0],
        [1, "b"],
        ["c", 2],
    ]);

    expect(new Map().has()).toBeFalse();
    expect(new Map([{}]).has()).toBeTrue();
    expect(map.has("a")).toBeTrue();
    expect(map.has(1)).toBeTrue();
    expect(map.has("serenity")).toBeFalse();
});
