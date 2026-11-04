describe("BigInt constant folding", () => {
    test("conversion to string is decimal", () => {
        expect(0x10n + "").toBe("16");
        expect(0x1_0n + "").toBe("16");
    });

    test("comparison with Number at the signed 64-bit boundary", () => {
        expect(9223372036854775808n == 9223372036854775808).toBeTrue();
        expect(9223372036854775807n < 9223372036854775808).toBeTrue();
        expect(18446744073709551616n == 18446744073709551616).toBeTrue();
    });
});
