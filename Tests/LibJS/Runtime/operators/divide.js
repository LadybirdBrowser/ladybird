// Used to prevent const folding from happening at compile time
const div = (lhs, rhs) => lhs / rhs;

test("basic division", () => {
    expect(div(10, 2)).toBe(5);
    expect(div(10, -1)).toBe(-10);
    expect(div(0, 10)).toBe(0);
    expect(div(10, 0)).toBe(Infinity);
});

test("optimized division", () => {
    // These are specially optimized operations which need extra tests
    const divide_by_one = x => x / 1;
    const divide_by_negative_one = x => x / -1;

    expect(divide_by_one(0)).toBe(0);
    expect(divide_by_one(10)).toBe(10);
    expect(divide_by_one(-10)).toBe(-10);
    expect(divide_by_one(NaN)).toBeNaN();
    expect(divide_by_one("123")).toBe(123);

    expect(divide_by_negative_one(0)).toBe(-0);
    expect(divide_by_negative_one(10)).toBe(-10);
    expect(divide_by_negative_one(-10)).toBe(10);
    expect(divide_by_negative_one(NaN)).toBeNaN();
    expect(divide_by_negative_one("123")).toBe(-123);
});
