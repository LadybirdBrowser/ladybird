describe("errors", () => {
    test("invalid index", () => {
        expect(() => {
            BigInt.asUintN(-1, 0n);
        }).toThrowWithMessage(RangeError, "Index must be a positive integer");

        expect(() => {
            BigInt.asUintN(Symbol(), 0n);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });

    test("invalid BigInt", () => {
        expect(() => {
            BigInt.asUintN(1, 1);
        }).toThrowWithMessage(TypeError, "Cannot convert number to BigInt");

        expect(() => {
            BigInt.asUintN(1, Symbol());
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to BigInt");

        expect(() => {
            BigInt.asUintN(1, "foo");
        }).toThrowWithMessage(SyntaxError, "Invalid value for BigInt: foo");
    });
});

describe("correct behavior", () => {
    test("basic functionality", () => {
        expect(BigInt.asUintN(0, -2n)).toBe(0n);
        expect(BigInt.asUintN(0, -1n)).toBe(0n);
        expect(BigInt.asUintN(0, 0n)).toBe(0n);
        expect(BigInt.asUintN(0, 1n)).toBe(0n);
        expect(BigInt.asUintN(0, 2n)).toBe(0n);

        expect(BigInt.asUintN(1, -3n)).toBe(1n);
        expect(BigInt.asUintN(1, -2n)).toBe(0n);
        expect(BigInt.asUintN(1, -1n)).toBe(1n);
        expect(BigInt.asUintN(1, 0n)).toBe(0n);
        expect(BigInt.asUintN(1, 1n)).toBe(1n);
        expect(BigInt.asUintN(1, 2n)).toBe(0n);
        expect(BigInt.asUintN(1, 3n)).toBe(1n);

        expect(BigInt.asUintN(2, -3n)).toBe(1n);
        expect(BigInt.asUintN(2, -2n)).toBe(2n);
        expect(BigInt.asUintN(2, -1n)).toBe(3n);
        expect(BigInt.asUintN(2, 0n)).toBe(0n);
        expect(BigInt.asUintN(2, 1n)).toBe(1n);
        expect(BigInt.asUintN(2, 2n)).toBe(2n);
        expect(BigInt.asUintN(2, 3n)).toBe(3n);

        expect(BigInt.asUintN(4, -3n)).toBe(13n);
        expect(BigInt.asUintN(4, -2n)).toBe(14n);
        expect(BigInt.asUintN(4, -1n)).toBe(15n);
        expect(BigInt.asUintN(4, 0n)).toBe(0n);
        expect(BigInt.asUintN(4, 1n)).toBe(1n);
        expect(BigInt.asUintN(4, 2n)).toBe(2n);
        expect(BigInt.asUintN(4, 3n)).toBe(3n);

        const extremelyBigInt = 123456789123456789123456789123456789123456789123456789n;

        expect(BigInt.asUintN(0, extremelyBigInt)).toBe(0n);
        expect(BigInt.asUintN(1, extremelyBigInt)).toBe(1n);
        expect(BigInt.asUintN(2, extremelyBigInt)).toBe(1n);
        expect(BigInt.asUintN(4, extremelyBigInt)).toBe(5n);
        expect(BigInt.asUintN(128, extremelyBigInt)).toBe(241220992521549204068304577237384191765n);
        expect(BigInt.asUintN(256, extremelyBigInt)).toBe(extremelyBigInt);

        expect(BigInt.asUintN(0, -extremelyBigInt)).toBe(0n);
        expect(BigInt.asUintN(1, -extremelyBigInt)).toBe(1n);
        expect(BigInt.asUintN(2, -extremelyBigInt)).toBe(3n);
        expect(BigInt.asUintN(4, -extremelyBigInt)).toBe(11n);
        expect(BigInt.asUintN(128, -extremelyBigInt)).toBe(99061374399389259395070030194384019691n);
        expect(BigInt.asUintN(256, -extremelyBigInt)).toBe(
            115792089237316195423570861551898784396480861208851440582668460551124006183147n
        );
    });
});
