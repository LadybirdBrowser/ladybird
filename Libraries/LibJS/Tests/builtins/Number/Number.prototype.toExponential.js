describe("errors", () => {
    test("must be called with numeric |this|", () => {
        [true, [], {}, Symbol("foo"), "bar", 1n].forEach(value => {
            expect(() => {
                Number.prototype.toExponential.call(value);
            }).toThrowWithMessage(TypeError, "Not an object of type Number");
        });
    });

    test("fraction digits must be coercible to a number", () => {
        expect(() => {
            (0).toExponential(Symbol("foo"));
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            (0).toExponential(1n);
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");
    });

    test("out of range fraction digits", () => {
        [-Infinity, -1, 101, Infinity].forEach(value => {
            expect(() => {
                (0).toExponential(value);
            }).toThrowWithMessage(
                RangeError,
                "Fraction Digits must be an integer no less than 0, and no greater than 100"
            );
        });
    });
});

describe("correct behavior", () => {
    test("special values", () => {
        [
            [Infinity, 6, "Infinity"],
            [-Infinity, 7, "-Infinity"],
            [NaN, 8, "NaN"],
            [0, 0, "0e+0"],
            [0, 1, "0.0e+0"],
            [0, 3, "0.000e+0"],
        ].forEach(test => {
            expect(test[0].toExponential(test[1])).toBe(test[2]);
        });
    });

    test("zero exponent", () => {
        [
            [1, 0, "1e+0"],
            [5, 1, "5.0e+0"],
            [9, 3, "9.000e+0"],

            // Disabled for now due to: https://github.com/SerenityOS/serenity/issues/15924
            // [3, 100, "3." + "0".repeat(100) + "e+0"],
        ].forEach(test => {
            expect(test[0].toExponential(test[1])).toBe(test[2]);
        });
    });

    test("positive exponent", () => {
        [
            [12, 0, "1e+1"],
            [345, 1, "3.5e+2"],
            [6789, 3, "6.789e+3"],
        ].forEach(test => {
            expect(test[0].toExponential(test[1])).toBe(test[2]);
        });
    });

    test("negative exponent", () => {
        [
            [0.12, 0, "1e-1"],
            [0.0345, 1, "3.5e-2"],
            [0.006789, 3, "6.789e-3"],
        ].forEach(test => {
            expect(test[0].toExponential(test[1])).toBe(test[2]);
        });
    });

    test("undefined precision", () => {
        [
            [123.456, "1.23456e+2"],
            [13, "1.3e+1"],
            [100, "1e+2"],
            [345, "3.45e+2"],
            [6789, "6.789e+3"],
            [0.13, "1.3e-1"],
            [0.0345, "3.45e-2"],
            [0.006789, "6.789e-3"],
            [1.1e-32, "1.1e-32"],
            [123.456, "1.23456e+2"],
        ].forEach(test => {
            expect(test[0].toExponential()).toBe(test[1]);
        });
    });
});
