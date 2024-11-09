describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Intl.PluralRules();
        }).toThrowWithMessage(TypeError, "Intl.PluralRules constructor must be called with 'new'");
    });

    test("options is an invalid type", () => {
        expect(() => {
            new Intl.PluralRules("en", null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("localeMatcher option is invalid ", () => {
        expect(() => {
            new Intl.PluralRules("en", { localeMatcher: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option localeMatcher");
    });

    test("type option is invalid ", () => {
        expect(() => {
            new Intl.PluralRules("en", { type: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option type");
    });

    test("minimumIntegerDigits option is invalid ", () => {
        expect(() => {
            new Intl.PluralRules("en", { minimumIntegerDigits: 1n });
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");

        expect(() => {
            new Intl.PluralRules("en", { minimumIntegerDigits: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 1 and 21");

        expect(() => {
            new Intl.PluralRules("en", { minimumIntegerDigits: 0 });
        }).toThrowWithMessage(RangeError, "Value 0 is NaN or is not between 1 and 21");

        expect(() => {
            new Intl.PluralRules("en", { minimumIntegerDigits: 22 });
        }).toThrowWithMessage(RangeError, "Value 22 is NaN or is not between 1 and 21");
    });

    test("minimumFractionDigits option is invalid ", () => {
        expect(() => {
            new Intl.PluralRules("en", { minimumFractionDigits: 1n });
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");

        expect(() => {
            new Intl.PluralRules("en", { minimumFractionDigits: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 0 and 100");

        expect(() => {
            new Intl.PluralRules("en", { minimumFractionDigits: -1 });
        }).toThrowWithMessage(RangeError, "Value -1 is NaN or is not between 0 and 100");

        expect(() => {
            new Intl.PluralRules("en", { minimumFractionDigits: 101 });
        }).toThrowWithMessage(RangeError, "Value 101 is NaN or is not between 0 and 100");
    });

    test("maximumFractionDigits option is invalid ", () => {
        expect(() => {
            new Intl.PluralRules("en", { maximumFractionDigits: 1n });
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");

        expect(() => {
            new Intl.PluralRules("en", { maximumFractionDigits: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 0 and 100");

        expect(() => {
            new Intl.PluralRules("en", { maximumFractionDigits: -1 });
        }).toThrowWithMessage(RangeError, "Value -1 is NaN or is not between 0 and 100");

        expect(() => {
            new Intl.PluralRules("en", { maximumFractionDigits: 101 });
        }).toThrowWithMessage(RangeError, "Value 101 is NaN or is not between 0 and 100");

        expect(() => {
            new Intl.PluralRules("en", { minimumFractionDigits: 10, maximumFractionDigits: 5 });
        }).toThrowWithMessage(RangeError, "Minimum value 10 is larger than maximum value 5");
    });

    test("minimumSignificantDigits option is invalid ", () => {
        expect(() => {
            new Intl.PluralRules("en", { minimumSignificantDigits: 1n });
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");

        expect(() => {
            new Intl.PluralRules("en", { minimumSignificantDigits: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 1 and 21");

        expect(() => {
            new Intl.PluralRules("en", { minimumSignificantDigits: 0 });
        }).toThrowWithMessage(RangeError, "Value 0 is NaN or is not between 1 and 21");

        expect(() => {
            new Intl.PluralRules("en", { minimumSignificantDigits: 22 });
        }).toThrowWithMessage(RangeError, "Value 22 is NaN or is not between 1 and 21");
    });

    test("maximumSignificantDigits option is invalid ", () => {
        expect(() => {
            new Intl.PluralRules("en", { maximumSignificantDigits: 1n });
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");

        expect(() => {
            new Intl.PluralRules("en", { maximumSignificantDigits: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 1 and 21");

        expect(() => {
            new Intl.PluralRules("en", { maximumSignificantDigits: 0 });
        }).toThrowWithMessage(RangeError, "Value 0 is NaN or is not between 1 and 21");

        expect(() => {
            new Intl.PluralRules("en", { maximumSignificantDigits: 22 });
        }).toThrowWithMessage(RangeError, "Value 22 is NaN or is not between 1 and 21");
    });

    test("roundingPriority option is invalid", () => {
        expect(() => {
            new Intl.PluralRules("en", { roundingPriority: "hello!" });
        }).toThrowWithMessage(
            RangeError,
            "hello! is not a valid value for option roundingPriority"
        );
    });

    test("roundingMode option is invalid", () => {
        expect(() => {
            new Intl.PluralRules("en", { roundingMode: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option roundingMode");
    });

    test("roundingIncrement option is invalid", () => {
        expect(() => {
            new Intl.PluralRules("en", { roundingIncrement: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 1 and 5000");

        expect(() => {
            new Intl.PluralRules("en", { roundingIncrement: 0 });
        }).toThrowWithMessage(RangeError, "Value 0 is NaN or is not between 1 and 5000");

        expect(() => {
            new Intl.PluralRules("en", { roundingIncrement: 5001 });
        }).toThrowWithMessage(RangeError, "Value 5001 is NaN or is not between 1 and 5000");

        expect(() => {
            new Intl.PluralRules("en", { roundingIncrement: 3 });
        }).toThrowWithMessage(RangeError, "3 is not a valid rounding increment");

        expect(() => {
            new Intl.PluralRules("en", { roundingIncrement: 5, minimumSignificantDigits: 1 });
        }).toThrowWithMessage(
            TypeError,
            "5 is not a valid rounding increment for rounding type significantDigits"
        );

        expect(() => {
            new Intl.PluralRules("en", {
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
            new Intl.PluralRules("en", { trailingZeroDisplay: "hello!" });
        }).toThrowWithMessage(
            RangeError,
            "hello! is not a valid value for option trailingZeroDisplay"
        );
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Intl.PluralRules).toHaveLength(0);
    });

    test("all valid localeMatcher options", () => {
        ["lookup", "best fit"].forEach(localeMatcher => {
            expect(() => {
                new Intl.PluralRules("en", { localeMatcher: localeMatcher });
            }).not.toThrow();
        });
    });

    test("all valid type options", () => {
        ["cardinal", "ordinal"].forEach(type => {
            expect(() => {
                new Intl.PluralRules("en", { type: type });
            }).not.toThrow();
        });
    });

    test("all valid minimumIntegerDigits options", () => {
        for (let i = 1; i <= 21; ++i) {
            expect(() => {
                new Intl.PluralRules("en", { minimumIntegerDigits: i });
            }).not.toThrow();
        }
    });

    test("all valid minimumFractionDigits options", () => {
        for (let i = 0; i <= 100; ++i) {
            expect(() => {
                new Intl.PluralRules("en", { minimumFractionDigits: i });
            }).not.toThrow();
        }
    });

    test("all valid maximumFractionDigits options", () => {
        for (let i = 0; i <= 100; ++i) {
            expect(() => {
                new Intl.PluralRules("en", { maximumFractionDigits: i });
            }).not.toThrow();
        }
    });

    test("all valid minimumSignificantDigits options", () => {
        for (let i = 1; i <= 21; ++i) {
            expect(() => {
                new Intl.PluralRules("en", { minimumSignificantDigits: i });
            }).not.toThrow();
        }
    });

    test("all valid maximumSignificantDigits options", () => {
        for (let i = 1; i <= 21; ++i) {
            expect(() => {
                new Intl.PluralRules("en", { maximumSignificantDigits: i });
            }).not.toThrow();
        }
    });

    test("all valid roundingPriority options", () => {
        ["auto", "morePrecision", "lessPrecision"].forEach(roundingPriority => {
            expect(() => {
                new Intl.PluralRules("en", { roundingPriority: roundingPriority });
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
                new Intl.PluralRules("en", { roundingMode: roundingMode });
            }).not.toThrow();
        });
    });

    test("all valid roundingIncrement options", () => {
        [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000].forEach(
            roundingIncrement => {
                expect(() => {
                    new Intl.PluralRules("en", { roundingIncrement: roundingIncrement });
                }).not.toThrow();
            }
        );
    });

    test("all valid trailingZeroDisplay options", () => {
        ["auto", "stripIfInteger"].forEach(trailingZeroDisplay => {
            expect(() => {
                new Intl.PluralRules("en", { trailingZeroDisplay: trailingZeroDisplay });
            }).not.toThrow();
        });
    });
});
