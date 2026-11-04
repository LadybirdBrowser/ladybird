describe("BigInt constant folding", () => {
    test("conversion to string is decimal", () => {
        expect(0x10n + "").toBe("16");
        expect(0x1_0n + "").toBe("16");
    });
});
