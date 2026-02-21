describe("errors", () => {
    test("must be called with numeric |this|", () => {
        [true, [], {}, Symbol("foo"), "bar", 1n].forEach(value => {
            expect(() => {
                Number.prototype.toPrecision.call(value);
            }).toThrowWithMessage(TypeError, "Not an object of type Number");
        });
    });

    test("precision must be coercible to a number", () => {
        expect(() => {
            (0).toPrecision(Symbol("foo"));
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            (0).toPrecision(1n);
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");
    });

    test("out of range precision", () => {
        [-Infinity, 0, 101, Infinity].forEach(value => {
            expect(() => {
                (0).toPrecision(value);
            }).toThrowWithMessage(RangeError, "Precision must be an integer no less than 1, and no greater than 100");
        });
    });
});

describe("correct behavior", () => {
    test("special values", () => {
        [
            [Infinity, 6, "Infinity"],
            [-Infinity, 7, "-Infinity"],
            [NaN, 8, "NaN"],
            [0, 1, "0"],
            [0, 3, "0.00"],
            [0, 5, "0.0000"],
        ].forEach(test => {
            expect(test[0].toPrecision(test[1])).toBe(test[2]);
        });
    });

    test("undefined precision yields plain number-to-string conversion", () => {
        [
            [123, undefined, "123"],
            [3.14, undefined, "3.14"],
        ].forEach(test => {
            expect(test[0].toPrecision(test[1])).toBe(test[2]);
        });
    });

    test("formatted as exponential string", () => {
        [
            // exponent < -6
            [0.0000002, 5, "2.0000e-7"],
            [0.00000000189, 3, "1.89e-9"],
            [0.00000000189, 2, "1.9e-9"],

            // exponent >= precision
            [100, 1, "1e+2"],
            [100, 2, "1.0e+2"],
            [1234589, 3, "1.23e+6"],
            [1234589, 4, "1.235e+6"],
            [1234589, 5, "1.2346e+6"],
        ].forEach(test => {
            expect(test[0].toPrecision(test[1])).toBe(test[2]);
        });
    });

    test("formatted without decimal", () => {
        [
            // exponent == precision - 1
            [1, 1, "1"],
            [123, 3, "123"],
            [123.45, 3, "123"],
        ].forEach(test => {
            expect(test[0].toPrecision(test[1])).toBe(test[2]);
        });
    });

    test("non-negative exponent", () => {
        [
            // exponent >= 0
            [1, 4, "1.000"],
            [123, 4, "123.0"],
            [123.45, 4, "123.5"],

            // Disabled for now due to: https://github.com/SerenityOS/serenity/issues/15924
            // [3, 100, "3." + "0".repeat(99)],
        ].forEach(test => {
            expect(test[0].toPrecision(test[1])).toBe(test[2]);
        });
    });

    test("negative exponent", () => {
        [
            // exponent < 0
            [0.1, 1, "0.1"],
            [0.0123, 3, "0.0123"],
            [0.0012345, 3, "0.00123"],
            [0.0012345, 4, "0.001234"],
        ].forEach(test => {
            expect(test[0].toPrecision(test[1])).toBe(test[2]);
        });
    });

    test("numbers exceeding the limits of a double", () => {
        [
            [1 + 11 / 31, 16, "1.354838709677419"],
            [-1.2345e27, 21, "-1.23449999999999996184e+27"],
            [0.000000000000000000001, 16, "9.999999999999999e-22"],
            [0.000000000000000000001, 17, "9.9999999999999991e-22"],
            [0.000000000000000000001, 18, "9.99999999999999908e-22"],
            [0.000000000000000000001, 19, "9.999999999999999075e-22"],
            [0.000000000000000000001, 20, "9.9999999999999990754e-22"],
            [0.000000000000000000001, 21, "9.99999999999999907537e-22"],
        ].forEach(test => {
            expect(test[0].toPrecision(test[1])).toBe(test[2]);
        });
    });
});
