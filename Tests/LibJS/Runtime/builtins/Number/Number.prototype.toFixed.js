describe("correct behavior", () => {
    test("length", () => {
        expect(Number.prototype.toFixed).toHaveLength(1);
    });

    test("basic functionality", () => {
        [
            [0, 5, "0.00000"],
            [Infinity, 6, "Infinity"],
            [-Infinity, 7, "-Infinity"],
            [NaN, 8, "NaN"],
            [12.81646112, 3, "12.816"],
            [84.23, 4, "84.2300"],
            [3.00003, 5, "3.00003"],
            [0.00006, 4, "0.0001"],
            // Numbers >= 1e+21
            [1e21, 5, "1e+21"],
            [1e22, 0, "1e+22"],
            // undefined, null and NaN are treated as 0 due to toFixed using ToIntegerOrInfinity.
            [1.1, undefined, "1"],
            [1.1, null, "1"],
            [1.1, NaN, "1"],
        ].forEach(testCase => {
            expect(testCase[0].toFixed(testCase[1])).toBe(testCase[2]);
        });
    });

    test("decimal fixed digits gets converted to int", () => {
        expect((30.521).toFixed(1.9)).toBe("30.5");
        expect((30.521).toFixed(2.2)).toBe("30.52");
    });

    test("rounding", () => {
        [
            // Exact ties round away from zero.
            [0.125, 2, "0.13"],
            [2.5, 0, "3"],
            [-2.5, 0, "-3"],
            // 1.005 is slightly less than its decimal spelling.
            [1.005, 2, "1.00"],
            [5e-26, 25, "0.0000000000000000000000001"],
            [-1e-7, 3, "-0.000"],
            [-0, 2, "0.00"],
        ].forEach(testCase => {
            expect(testCase[0].toFixed(testCase[1])).toBe(testCase[2]);
        });
    });

    test("scaled integer size boundaries", () => {
        [
            [123.456, 27, "123.456000000000003069544618484"],
            [123.456, 28, "123.4560000000000030695446184836"],
            [1e-10, 27, "0.000000000100000000000000004"],
            [1.8e17, 2, "180000000000000000.00"],
            [1e20, 2, "100000000000000000000.00"],
            [2 ** 52, 1, "4503599627370496.0"],
            [2 ** 53, 0, "9007199254740992"],
        ].forEach(testCase => {
            expect(testCase[0].toFixed(testCase[1])).toBe(testCase[2]);
        });
    });
});

describe("large number of digits", () => {
    test("maximum", () => {
        expect((1).toFixed(100)).toBe(
            "1.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        );
        expect((-3).toFixed(100)).toBe(
            "-3.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        );
    });

    test("fractional values", () => {
        expect((1.5).toFixed(30)).toBe("1.500000000000000000000000000000");
        expect((3.141592653589793).toFixed(50)).toBe("3.14159265358979311599796346854418516159057617187500");
        expect((123456.78).toFixed(50)).toBe("123456.77999999999883584678173065185546875000000000000000");
        expect(Number.MIN_VALUE.toFixed(100)).toBe(
            "0.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        );
        expect((-Number.MIN_VALUE).toFixed(100)).toBe(
            "-0.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        );
    });
});

describe("errors", () => {
    test("must be called with numeric |this|", () => {
        [true, [], {}, Symbol("foo"), "bar", 1n].forEach(value => {
            expect(() => Number.prototype.toFixed.call(value)).toThrowWithMessage(
                TypeError,
                "Not an object of type Number"
            );
        });
    });

    test("fixed digits RangeError", () => {
        [-Infinity, -5, 105, Infinity].forEach(value => {
            expect(() => (0).toFixed(value)).toThrow(RangeError);
        });
    });
});
