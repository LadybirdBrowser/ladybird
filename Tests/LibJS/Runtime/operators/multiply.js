// Used to prevent const folding from happening at compile time
const mul = (lhs, rhs) => lhs * rhs;

test("basic multiplication", () => {
    expect(mul(2, 3)).toBe(6);
    expect(mul(10, -1)).toBe(-10);
    expect(mul(0, 10)).toBe(0);
});

test("optimized multiplication", () => {
    // These are specially optimized operations which need extra tests
    const mult_by_one = x => x * 1;
    const mult_by_one_alt = x => 1 * x;
    const mult_by_negative_one = x => x * -1;
    const mult_by_negative_one_alt = x => -1 * x;

    expect(mult_by_one(0)).toBe(0);
    expect(mult_by_one(10)).toBe(10);
    expect(mult_by_one(-10)).toBe(-10);
    expect(mult_by_one(NaN)).toBeNaN();
    expect(mult_by_one("123")).toBe(123);

    expect(mult_by_one_alt(0)).toBe(0);
    expect(mult_by_one_alt(10)).toBe(10);
    expect(mult_by_one_alt(-10)).toBe(-10);
    expect(mult_by_one_alt(NaN)).toBeNaN();
    expect(mult_by_one_alt("123")).toBe(123);

    expect(mult_by_negative_one(0)).toBe(-0);
    expect(mult_by_negative_one(10)).toBe(-10);
    expect(mult_by_negative_one(-10)).toBe(10);
    expect(mult_by_negative_one(NaN)).toBeNaN();
    expect(mult_by_negative_one("123")).toBe(-123);

    expect(mult_by_negative_one_alt(0)).toBe(-0);
    expect(mult_by_negative_one_alt(10)).toBe(-10);
    expect(mult_by_negative_one_alt(-10)).toBe(10);
    expect(mult_by_negative_one_alt(NaN)).toBeNaN();
    expect(mult_by_negative_one_alt("123")).toBe(-123);
});
