describe("errors", () => {
    test("invalid index", () => {
        expect(() => {
            BigInt.asIntN(-1, 0n);
        }).toThrowWithMessage(RangeError, "Index must be a positive integer");

        expect(() => {
            BigInt.asIntN(Symbol(), 0n);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });

    test("invalid BigInt", () => {
        expect(() => {
            BigInt.asIntN(1, 1);
        }).toThrowWithMessage(TypeError, "Cannot convert number to BigInt");

        expect(() => {
            BigInt.asIntN(1, Symbol());
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to BigInt");

        expect(() => {
            BigInt.asIntN(1, "foo");
        }).toThrowWithMessage(SyntaxError, "Invalid value for BigInt: foo");
    });
});

describe("correct behavior", () => {
    test("length is 2", () => {
        expect(BigInt.asIntN).toHaveLength(2);
    });

    test("basic functionality", () => {
        expect(BigInt.asIntN(0, -2n)).toBe(0n);
        expect(BigInt.asIntN(0, -1n)).toBe(0n);
        expect(BigInt.asIntN(0, 0n)).toBe(0n);
        expect(BigInt.asIntN(0, 1n)).toBe(0n);
        expect(BigInt.asIntN(0, 2n)).toBe(0n);

        expect(BigInt.asIntN(1, -3n)).toBe(-1n);
        expect(BigInt.asIntN(1, -2n)).toBe(0n);
        expect(BigInt.asIntN(1, -1n)).toBe(-1n);
        expect(BigInt.asIntN(1, 0n)).toBe(0n);
        expect(BigInt.asIntN(1, 1n)).toBe(-1n);
        expect(BigInt.asIntN(1, 2n)).toBe(0n);
        expect(BigInt.asIntN(1, 3n)).toBe(-1n);

        expect(BigInt.asIntN(2, -3n)).toBe(1n);
        expect(BigInt.asIntN(2, -2n)).toBe(-2n);
        expect(BigInt.asIntN(2, -1n)).toBe(-1n);
        expect(BigInt.asIntN(2, 0n)).toBe(0n);
        expect(BigInt.asIntN(2, 1n)).toBe(1n);
        expect(BigInt.asIntN(2, 2n)).toBe(-2n);
        expect(BigInt.asIntN(2, 3n)).toBe(-1n);

        expect(BigInt.asIntN(4, -3n)).toBe(-3n);
        expect(BigInt.asIntN(4, -2n)).toBe(-2n);
        expect(BigInt.asIntN(4, -1n)).toBe(-1n);
        expect(BigInt.asIntN(4, 0n)).toBe(0n);
        expect(BigInt.asIntN(4, 1n)).toBe(1n);
        expect(BigInt.asIntN(4, 2n)).toBe(2n);
        expect(BigInt.asIntN(4, 3n)).toBe(3n);

        const extremelyBigInt = 123456789123456789123456789123456789123456789123456789n;

        expect(BigInt.asIntN(0, extremelyBigInt)).toBe(0n);
        expect(BigInt.asIntN(1, extremelyBigInt)).toBe(-1n);
        expect(BigInt.asIntN(2, extremelyBigInt)).toBe(1n);
        expect(BigInt.asIntN(4, extremelyBigInt)).toBe(5n);
        expect(BigInt.asIntN(128, extremelyBigInt)).toBe(-99061374399389259395070030194384019691n);
        expect(BigInt.asIntN(256, extremelyBigInt)).toBe(extremelyBigInt);

        expect(BigInt.asIntN(0, -extremelyBigInt)).toBe(0n);
        expect(BigInt.asIntN(1, -extremelyBigInt)).toBe(-1n);
        expect(BigInt.asIntN(2, -extremelyBigInt)).toBe(-1n);
        expect(BigInt.asIntN(4, -extremelyBigInt)).toBe(-5n);
        expect(BigInt.asIntN(128, -extremelyBigInt)).toBe(99061374399389259395070030194384019691n);
        expect(BigInt.asIntN(256, -extremelyBigInt)).toBe(-extremelyBigInt);
    });
});
