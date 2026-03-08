test("int32 multiplication with negative results compares correctly", () => {
    var a = 1;
    var b = -1;
    var c = a * b;
    expect(c).toBe(-1);
    expect(c === -1).toBeTrue();

    var d = 3;
    var e = -7;
    var f = d * e;
    expect(f).toBe(-21);
    expect(f === -21).toBeTrue();

    // Compound assignment
    var x = 5;
    x *= -3;
    expect(x).toBe(-15);
    expect(x === -15).toBeTrue();

    // Two negatives
    var g = -4;
    var h = -5;
    expect(g * h).toBe(20);
    expect(g * h === 20).toBeTrue();
});
