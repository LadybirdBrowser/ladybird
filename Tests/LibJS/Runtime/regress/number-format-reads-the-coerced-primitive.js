describe("Intl.NumberFormat reads the coerced primitive, not the original value", () => {
    test("an object that coerces to a BigInt", () => {
        const object = {
            [Symbol.toPrimitive]() {
                return 123456789012345678901234567890n;
            },
        };

        expect(new Intl.NumberFormat("en").format(object)).toBe("123,456,789,012,345,678,901,234,567,890");
    });

    test("valueOf that returns a BigInt", () => {
        const object = {
            valueOf() {
                return -42n;
            },
        };

        expect(new Intl.NumberFormat("en").format(object)).toBe("-42");
    });
});
