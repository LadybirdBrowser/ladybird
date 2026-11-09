describe("correct behavior", () => {
    test("length", () => {
        expect(Number.prototype.toString).toHaveLength(1);
    });

    test("basic functionality", () => {
        [
            [+0, "0"],
            [-0, "0"],
            [Infinity, "Infinity"],
            [-Infinity, "-Infinity"],
            [NaN, "NaN"],
            [12, "12"],
            [93465, "93465"],
            [358000, "358000"],
            // Numbers above 2 ** 31 - 1 (Issue #3931)
            [2147483648, "2147483648"], // 2 ** 31
            [4294967295, "4294967295"], // 2 ** 32 - 1
            [18014398509481984, "18014398509481984"], // 2 ** 54
        ].forEach(testCase => {
            expect(testCase[0].toString()).toBe(testCase[1]);
        });
    });

    test("radix", () => {
        let number = 7857632;

        [
            [2, "11101111110010111100000"],
            [3, "112210012122102"],
            [4, "131332113200"],
            [5, "4002421012"],
            [6, "440225532"],
            [7, "123534356"],
            [8, "35762740"],
            [9, "15705572"],
            [10, "7857632"],
            [11, "4487612"],
            [12, "276b2a8"],
            [13, "18216b3"],
            [14, "10877d6"],
            [15, "a532c2"],
            [16, "77e5e0"],
            [17, "59160b"],
            [18, "42f5h2"],
            [19, "335b5b"],
            [20, "29241c"],
            [21, "1j89fk"],
            [22, "1bbkh2"],
            [23, "151ih4"],
            [24, "ng9h8"],
            [25, "k2m57"],
            [26, "h51ig"],
            [27, "el5hb"],
            [28, "clqdk"],
            [29, "b355o"],
            [30, "9l0l2"],
            [31, "8fng0"],
            [32, "7fpf0"],
            [33, "6klf2"],
            [34, "5tv8s"],
            [35, "589dr"],
            [36, "4oezk"],
        ].forEach(testCase => {
            expect(number.toString(testCase[0])).toBe(testCase[1]);
        });
    });

    test("fractional number with radix", () => {
        expect((0.5).toString(2)).toBe("0.1");
    });

    test("fractional radix conversion preserves significant digits", () => {
        const cases = [
            [0.07, 16, "0.11eb851eb851ec"],
            [0.62, 16, "0.9eb851eb851eb8"],
            [0.1, 16, "0.1999999999999a"],
            [0.01, 16, "0.028f5c28f5c28f6"],
            [90.12, 16, "5a.1eb851eb852"],
            [2 ** -60, 16, "0.000000000000001"],
            [Number.MIN_VALUE, 2, "0." + "0".repeat(1073) + "1"],
            [Number.MIN_VALUE, 16, "0." + "0".repeat(268) + "4"],
            [2 ** -1022, 2, "0." + "0".repeat(1021) + "1"],
            [0.1, 3, "0.0022002200220022002200220022002201"],
        ];
        for (const [number, radix, expected] of cases) {
            expect(number.toString(radix)).toBe(expected);
            expect((-number).toString(radix)).toBe("-" + expected);
        }
    });

    test("fractional radix conversion rounds at binary exponent boundaries", () => {
        const cases = [
            [0.5 - 2 ** -54, 3, "0.1111111111111111111111111111111111"],
            [0.5, 3, "0.1111111111111111111111111111111112"],
            [0.5 + 2 ** -53, 3, "0.111111111111111111111111111111112"],
            [0.1, 7, "0.04620462046204620463"],
            [2 ** -1022 - Number.MIN_VALUE, 16, "0." + "0".repeat(255) + "3ffffffffffffc"],
            [2 ** -1022 + Number.MIN_VALUE, 16, "0." + "0".repeat(255) + "40000000000004"],
            [2 ** 52 - 0.5, 16, "fffffffffffff.8"],
            [0.9999999999999999, 36, "0.zzzzzzzzzzl"],
            [1.0000000000000002, 36, "1.0000000001"],
        ];
        for (const [number, radix, expected] of cases) {
            expect(number.toString(radix)).toBe(expected);
            expect((-number).toString(radix)).toBe("-" + expected);
        }
    });

    test("decimal radix gets converted to int", () => {
        expect((30).toString(10.1)).toBe("30");
        expect((30).toString(10.9)).toBe("30");
    });

    test("extremely large number with radix", () => {
        // Example number generated from an ID generator in Raygun4js: https://github.com/MindscapeHQ/raygun4js/blob/777ba6e3cdede42011aa0b7f019473de6380009b/src/raygun.rum/index.js#L1204
        const number = 2.2171010912173817e51;

        for (let radix = 2; radix <= 36; ++radix) {
            if (radix === 10) expect(number.toString(radix)).toBe("2.2171010912173817e+51");
            else expect(number.toString(radix)).toBe(BigInt(number).toString(radix));
        }
    });
});

describe("errors", () => {
    test("must be called with numeric |this|", () => {
        [true, [], {}, Symbol("foo"), "bar", 1n].forEach(value => {
            expect(() => Number.prototype.toString.call(value)).toThrowWithMessage(
                TypeError,
                "Not an object of type Number"
            );
        });
    });

    test("radix RangeError", () => {
        [0, 1, 37, 100].forEach(value => {
            expect(() => (0).toString(value)).toThrow(RangeError);
        });
    });
});

test("integral doubles", () => {
    expect((2 ** 31).toString()).toBe("2147483648");
    expect((-(2 ** 31) - 1).toString()).toBe("-2147483649");
    expect((2 ** 52).toString()).toBe("4503599627370496");
    expect((2 ** 53 - 1).toString()).toBe("9007199254740991");
    expect((-(2 ** 53) + 1).toString()).toBe("-9007199254740991");
    expect((2 ** 53).toString()).toBe("9007199254740992");
    expect((1e20).toString()).toBe("100000000000000000000");
    expect((1e21).toString()).toBe("1e+21");
    expect(String(-0)).toBe("0");
    expect([2 ** 40, -(2 ** 40), 4.0].join()).toBe("1099511627776,-1099511627776,4");
});

test("integer digits of large values in a radix", () => {
    expect((1e17).toString(36)).toBe("rcn1hssighs");
    expect((1e21).toString(36)).toBe("5v1j4f4ds79m9s");
    for (const value of [2 ** 53 + 2, 1e17, 1e21, 2 ** 64, 1e300, Number.MAX_VALUE]) {
        for (const radix of [2, 3, 7, 36]) {
            expect(value.toString(radix)).toBe(BigInt(value).toString(radix));
            expect((-value).toString(radix)).toBe((-BigInt(value)).toString(radix));
        }
    }
});
