describe("correct behavior", () => {
    test("basic functionality", () => {
        expect(BigInt).toHaveLength(1);
        expect(BigInt.name).toBe("BigInt");
    });

    test("constructor with numbers", () => {
        expect(BigInt(0)).toBe(0n);
        expect(BigInt(1)).toBe(1n);
        expect(BigInt(+1)).toBe(1n);
        expect(BigInt(-1)).toBe(-1n);
        expect(BigInt(123n)).toBe(123n);
    });

    test("constructor with strings", () => {
        expect(BigInt("")).toBe(0n);
        expect(BigInt("0")).toBe(0n);
        expect(BigInt("1")).toBe(1n);
        expect(BigInt("+1")).toBe(1n);
        expect(BigInt("-1")).toBe(-1n);
        expect(BigInt("-1")).toBe(-1n);
        expect(BigInt("42")).toBe(42n);
        expect(BigInt("  \n  00100  \n  ")).toBe(100n);
        expect(BigInt("3323214327642987348732109829832143298746432437532197321")).toBe(
            3323214327642987348732109829832143298746432437532197321n
        );
    });

    test("constructor with objects", () => {
        expect(BigInt([])).toBe(0n);
    });

    test("base-2 strings", () => {
        expect(BigInt("0b0")).toBe(0n);
        expect(BigInt("0B0")).toBe(0n);
        expect(BigInt("0b1")).toBe(1n);
        expect(BigInt("0B1")).toBe(1n);
        expect(BigInt("0b10")).toBe(2n);
        expect(BigInt("0B10")).toBe(2n);
        expect(BigInt(`0b${"1".repeat(100)}`)).toBe(1267650600228229401496703205375n);
    });

    test("base-8 strings", () => {
        expect(BigInt("0o0")).toBe(0n);
        expect(BigInt("0O0")).toBe(0n);
        expect(BigInt("0o1")).toBe(1n);
        expect(BigInt("0O1")).toBe(1n);
        expect(BigInt("0o7")).toBe(7n);
        expect(BigInt("0O7")).toBe(7n);
        expect(BigInt("0o10")).toBe(8n);
        expect(BigInt("0O10")).toBe(8n);
        expect(BigInt(`0o1${"7".repeat(33)}`)).toBe(1267650600228229401496703205375n);
    });

    test("base-16 strings", () => {
        expect(BigInt("0x0")).toBe(0n);
        expect(BigInt("0X0")).toBe(0n);
        expect(BigInt("0x1")).toBe(1n);
        expect(BigInt("0X1")).toBe(1n);
        expect(BigInt("0xf")).toBe(15n);
        expect(BigInt("0Xf")).toBe(15n);
        expect(BigInt("0x10")).toBe(16n);
        expect(BigInt("0X10")).toBe(16n);
        expect(BigInt(`0x${"f".repeat(25)}`)).toBe(1267650600228229401496703205375n);
    });

    test("only coerces value once", () => {
        let calls = 0;
        const value = {
            [Symbol.toPrimitive]() {
                expect(calls).toBe(0);
                ++calls;
                return "123";
            },
        };

        expect(BigInt(value)).toEqual(123n);
        expect(calls).toBe(1);
    });
});

describe("errors", () => {
    test('cannot be constructed with "new"', () => {
        expect(() => {
            new BigInt();
        }).toThrowWithMessage(TypeError, "BigInt is not a constructor");
    });

    test("invalid arguments", () => {
        expect(() => {
            BigInt(null);
        }).toThrowWithMessage(TypeError, "Cannot convert null to BigInt");

        expect(() => {
            BigInt(undefined);
        }).toThrowWithMessage(TypeError, "Cannot convert undefined to BigInt");

        expect(() => {
            BigInt(Symbol());
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to BigInt");

        ["foo", "123n", "1+1", {}, function () {}].forEach(value => {
            expect(() => {
                BigInt(value);
            }).toThrowWithMessage(SyntaxError, `Invalid value for BigInt: ${value}`);
        });
    });

    test("invalid numeric arguments", () => {
        [1.23, Infinity, -Infinity, NaN].forEach(value => {
            expect(() => {
                BigInt(value);
            }).toThrowWithMessage(RangeError, "Cannot convert non-integral number to BigInt");
        });
    });

    test("invalid string for base", () => {
        ["0b", "0b2", "0B02", "-0b1", "-0B1"].forEach(value => {
            expect(() => {
                BigInt(value);
            }).toThrowWithMessage(SyntaxError, `Invalid value for BigInt: ${value}`);
        });

        ["0o", "0o8", "0O08", "-0o1", "-0O1"].forEach(value => {
            expect(() => {
                BigInt(value);
            }).toThrowWithMessage(SyntaxError, `Invalid value for BigInt: ${value}`);
        });

        ["0x", "0xg", "0X0g", "-0x1", "-0X1"].forEach(value => {
            expect(() => {
                BigInt(value);
            }).toThrowWithMessage(SyntaxError, `Invalid value for BigInt: ${value}`);
        });

        ["a", "-1a"].forEach(value => {
            expect(() => {
                BigInt(value);
            }).toThrowWithMessage(SyntaxError, `Invalid value for BigInt: ${value}`);
        });
    });
});
