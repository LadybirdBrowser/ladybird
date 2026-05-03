test("long source containing non-ASCII text parses", () => {
    var value = "ę" + "x".repeat(102);

    expect(value.length).toBe(103);
});
