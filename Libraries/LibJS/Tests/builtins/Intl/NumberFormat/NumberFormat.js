describe("errors", () => {
    test("structurally invalid tag", () => {
        expect(() => {
            new Intl.NumberFormat("root");
        }).toThrowWithMessage(RangeError, "root is not a structurally valid language tag");

        expect(() => {
            new Intl.NumberFormat("en-");
        }).toThrowWithMessage(RangeError, "en- is not a structurally valid language tag");

        expect(() => {
            new Intl.NumberFormat("Latn");
        }).toThrowWithMessage(RangeError, "Latn is not a structurally valid language tag");

        expect(() => {
            new Intl.NumberFormat("en-u-aa-U-aa");
        }).toThrowWithMessage(RangeError, "en-u-aa-U-aa is not a structurally valid language tag");
    });

    test("options is an invalid type", () => {
        expect(() => {
            new Intl.NumberFormat("en", null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("localeMatcher option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { localeMatcher: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option localeMatcher");
    });

    test("numberingSystem option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { numberingSystem: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option numberingSystem");
    });

    test("style option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { style: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option style");
    });

    test("currency option is undefined when required ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { style: "currency" });
        }).toThrowWithMessage(
            TypeError,
            "Option currency must be defined when option style is currency"
        );
    });

    test("currency option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { currency: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option currency");
    });

    test("currencyDisplay option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { currencyDisplay: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option currencyDisplay");
    });

    test("currencySign option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { currencySign: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option currencySign");
    });

    test("unit option is undefined when required ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { style: "unit" });
        }).toThrowWithMessage(TypeError, "Option unit must be defined when option style is unit");
    });

    test("unit option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { unit: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option unit");

        expect(() => {
            new Intl.NumberFormat("en", { unit: "acre-bit" });
        }).toThrowWithMessage(RangeError, "acre-bit is not a valid value for option unit");

        expect(() => {
            new Intl.NumberFormat("en", { unit: "acre-per-bit-per-byte" });
        }).toThrowWithMessage(
            RangeError,
            "acre-per-bit-per-byte is not a valid value for option unit"
        );
    });

    test("unitDisplay option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { unitDisplay: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option unitDisplay");
    });

    test("notation option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { notation: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option notation");
    });

    test("minimumIntegerDigits option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { minimumIntegerDigits: 1n });
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");

        expect(() => {
            new Intl.NumberFormat("en", { minimumIntegerDigits: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 1 and 21");

        expect(() => {
            new Intl.NumberFormat("en", { minimumIntegerDigits: 0 });
        }).toThrowWithMessage(RangeError, "Value 0 is NaN or is not between 1 and 21");

        expect(() => {
            new Intl.NumberFormat("en", { minimumIntegerDigits: 22 });
        }).toThrowWithMessage(RangeError, "Value 22 is NaN or is not between 1 and 21");
    });

    test("minimumFractionDigits option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { minimumFractionDigits: 1n });
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");

        expect(() => {
            new Intl.NumberFormat("en", { minimumFractionDigits: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 0 and 100");

        expect(() => {
            new Intl.NumberFormat("en", { minimumFractionDigits: -1 });
        }).toThrowWithMessage(RangeError, "Value -1 is NaN or is not between 0 and 100");

        expect(() => {
            new Intl.NumberFormat("en", { minimumFractionDigits: 101 });
        }).toThrowWithMessage(RangeError, "Value 101 is NaN or is not between 0 and 100");
    });

    test("maximumFractionDigits option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { maximumFractionDigits: 1n });
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");

        expect(() => {
            new Intl.NumberFormat("en", { maximumFractionDigits: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 0 and 100");

        expect(() => {
            new Intl.NumberFormat("en", { maximumFractionDigits: -1 });
        }).toThrowWithMessage(RangeError, "Value -1 is NaN or is not between 0 and 100");

        expect(() => {
            new Intl.NumberFormat("en", { maximumFractionDigits: 101 });
        }).toThrowWithMessage(RangeError, "Value 101 is NaN or is not between 0 and 100");

        expect(() => {
            new Intl.NumberFormat("en", { minimumFractionDigits: 10, maximumFractionDigits: 5 });
        }).toThrowWithMessage(RangeError, "Minimum value 10 is larger than maximum value 5");
    });

    test("minimumSignificantDigits option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { minimumSignificantDigits: 1n });
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");

        expect(() => {
            new Intl.NumberFormat("en", { minimumSignificantDigits: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 1 and 21");

        expect(() => {
            new Intl.NumberFormat("en", { minimumSignificantDigits: 0 });
        }).toThrowWithMessage(RangeError, "Value 0 is NaN or is not between 1 and 21");

        expect(() => {
            new Intl.NumberFormat("en", { minimumSignificantDigits: 22 });
        }).toThrowWithMessage(RangeError, "Value 22 is NaN or is not between 1 and 21");
    });

    test("maximumSignificantDigits option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { maximumSignificantDigits: 1n });
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");

        expect(() => {
            new Intl.NumberFormat("en", { maximumSignificantDigits: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 1 and 21");

        expect(() => {
            new Intl.NumberFormat("en", { maximumSignificantDigits: 0 });
        }).toThrowWithMessage(RangeError, "Value 0 is NaN or is not between 1 and 21");

        expect(() => {
            new Intl.NumberFormat("en", { maximumSignificantDigits: 22 });
        }).toThrowWithMessage(RangeError, "Value 22 is NaN or is not between 1 and 21");
    });

    test("compactDisplay option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { compactDisplay: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option compactDisplay");
    });

    test("signDisplay option is invalid ", () => {
        expect(() => {
            new Intl.NumberFormat("en", { signDisplay: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option signDisplay");
    });

    test("useGrouping option is invalid", () => {
        expect(() => {
            new Intl.NumberFormat("en", { useGrouping: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option useGrouping");
    });

    test("roundingPriority option is invalid", () => {
        expect(() => {
            new Intl.NumberFormat("en", { roundingPriority: "hello!" });
        }).toThrowWithMessage(
            RangeError,
            "hello! is not a valid value for option roundingPriority"
        );
    });

    test("roundingMode option is invalid", () => {
        expect(() => {
            new Intl.NumberFormat("en", { roundingMode: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option roundingMode");
    });

    test("roundingIncrement option is invalid", () => {
        expect(() => {
            new Intl.NumberFormat("en", { roundingIncrement: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 1 and 5000");

        expect(() => {
            new Intl.NumberFormat("en", { roundingIncrement: 0 });
        }).toThrowWithMessage(RangeError, "Value 0 is NaN or is not between 1 and 5000");

        expect(() => {
            new Intl.NumberFormat("en", { roundingIncrement: 5001 });
        }).toThrowWithMessage(RangeError, "Value 5001 is NaN or is not between 1 and 5000");

        expect(() => {
            new Intl.NumberFormat("en", { roundingIncrement: 3 });
        }).toThrowWithMessage(RangeError, "3 is not a valid rounding increment");

        expect(() => {
            new Intl.NumberFormat("en", { roundingIncrement: 5, minimumSignificantDigits: 1 });
        }).toThrowWithMessage(
            TypeError,
            "5 is not a valid rounding increment for rounding type significantDigits"
        );

        expect(() => {
            new Intl.NumberFormat("en", {
                roundingIncrement: 5,
                minimumFractionDigits: 2,
                maximumFractionDigits: 3,
            });
        }).toThrowWithMessage(
            RangeError,
            "5 is not a valid rounding increment for inequal min/max fraction digits"
        );
    });

    test("trailingZeroDisplay option is invalid", () => {
        expect(() => {
            new Intl.NumberFormat("en", { trailingZeroDisplay: "hello!" });
        }).toThrowWithMessage(
            RangeError,
            "hello! is not a valid value for option trailingZeroDisplay"
        );
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Intl.NumberFormat).toHaveLength(0);
    });

    test("all valid localeMatcher options", () => {
        ["lookup", "best fit"].forEach(localeMatcher => {
            expect(() => {
                new Intl.NumberFormat("en", { localeMatcher: localeMatcher });
            }).not.toThrow();
        });
    });

    test("valid numberingSystem options", () => {
        ["latn", "arab", "abc-def-ghi"].forEach(numberingSystem => {
            expect(() => {
                new Intl.NumberFormat("en", { numberingSystem: numberingSystem });
            }).not.toThrow();
        });
    });

    test("all valid style options", () => {
        ["decimal", "percent"].forEach(style => {
            expect(() => {
                new Intl.NumberFormat("en", { style: style });
            }).not.toThrow();
        });

        expect(() => {
            new Intl.NumberFormat("en", { style: "currency", currency: "USD" });
        }).not.toThrow();

        expect(() => {
            new Intl.NumberFormat("en", { style: "unit", unit: "degree" });
        }).not.toThrow();
    });

    test("valid currency options", () => {
        ["USD", "EUR", "XAG"].forEach(currency => {
            expect(() => {
                new Intl.NumberFormat("en", { currency: currency });
            }).not.toThrow();
        });
    });

    test("all valid currencyDisplay options", () => {
        ["code", "symbol", "narrowSymbol", "name"].forEach(currencyDisplay => {
            expect(() => {
                new Intl.NumberFormat("en", { currencyDisplay: currencyDisplay });
            }).not.toThrow();
        });
    });

    test("all valid currencySign options", () => {
        ["standard", "accounting"].forEach(currencySign => {
            expect(() => {
                new Intl.NumberFormat("en", { currencySign: currencySign });
            }).not.toThrow();
        });
    });

    test("valid unit options", () => {
        ["acre", "acre-per-bit"].forEach(unit => {
            expect(() => {
                new Intl.NumberFormat("en", { unit: unit });
            }).not.toThrow();
        });
    });

    test("all valid unitDisplay options", () => {
        ["short", "narrow", "long"].forEach(unitDisplay => {
            expect(() => {
                new Intl.NumberFormat("en", { unitDisplay: unitDisplay });
            }).not.toThrow();
        });
    });

    test("all valid notation options", () => {
        ["standard", "scientific", "engineering", "compact"].forEach(notation => {
            expect(() => {
                new Intl.NumberFormat("en", { notation: notation });
            }).not.toThrow();
        });
    });

    test("all valid minimumIntegerDigits options", () => {
        for (let i = 1; i <= 21; ++i) {
            expect(() => {
                new Intl.NumberFormat("en", { minimumIntegerDigits: i });
            }).not.toThrow();
        }
    });

    test("all valid minimumFractionDigits options", () => {
        for (let i = 0; i <= 100; ++i) {
            expect(() => {
                new Intl.NumberFormat("en", { minimumFractionDigits: i });
            }).not.toThrow();
        }
    });

    test("all valid maximumFractionDigits options", () => {
        for (let i = 0; i <= 100; ++i) {
            expect(() => {
                new Intl.NumberFormat("en", { maximumFractionDigits: i });
            }).not.toThrow();
        }
    });

    test("all valid minimumSignificantDigits options", () => {
        for (let i = 1; i <= 21; ++i) {
            expect(() => {
                new Intl.NumberFormat("en", { minimumSignificantDigits: i });
            }).not.toThrow();
        }
    });

    test("all valid maximumSignificantDigits options", () => {
        for (let i = 1; i <= 21; ++i) {
            expect(() => {
                new Intl.NumberFormat("en", { maximumSignificantDigits: i });
            }).not.toThrow();
        }
    });

    test("all valid compactDisplay options", () => {
        ["short", "long"].forEach(compactDisplay => {
            expect(() => {
                new Intl.NumberFormat("en", { compactDisplay: compactDisplay });
            }).not.toThrow();
        });
    });

    test("all valid signDisplay options", () => {
        ["auto", "never", "always", "exceptZero", "negative"].forEach(signDisplay => {
            expect(() => {
                new Intl.NumberFormat("en", { signDisplay: signDisplay });
            }).not.toThrow();
        });
    });

    test("valid useGrouping options", () => {
        ["min2", "auto", "always", false, true, "false", "true", ""].forEach(useGrouping => {
            expect(() => {
                new Intl.NumberFormat("en", { useGrouping: useGrouping });
            }).not.toThrow();
        });
    });

    test("all valid roundingPriority options", () => {
        ["auto", "morePrecision", "lessPrecision"].forEach(roundingPriority => {
            expect(() => {
                new Intl.NumberFormat("en", { roundingPriority: roundingPriority });
            }).not.toThrow();
        });
    });

    test("all valid roundingMode options", () => {
        [
            "ceil",
            "floor",
            "expand",
            "trunc",
            "halfCeil",
            "halfFloor",
            "halfExpand",
            "halfTrunc",
            "halfEven",
        ].forEach(roundingMode => {
            expect(() => {
                new Intl.NumberFormat("en", { roundingMode: roundingMode });
            }).not.toThrow();
        });
    });

    test("all valid roundingIncrement options", () => {
        [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000].forEach(
            roundingIncrement => {
                expect(() => {
                    new Intl.NumberFormat("en", { roundingIncrement: roundingIncrement });
                }).not.toThrow();
            }
        );
    });

    test("all valid trailingZeroDisplay options", () => {
        ["auto", "stripIfInteger"].forEach(trailingZeroDisplay => {
            expect(() => {
                new Intl.NumberFormat("en", { trailingZeroDisplay: trailingZeroDisplay });
            }).not.toThrow();
        });
    });
});
