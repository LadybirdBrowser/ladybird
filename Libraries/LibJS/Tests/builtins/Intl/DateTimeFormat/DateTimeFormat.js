describe("errors", () => {
    test("structurally invalid tag", () => {
        expect(() => {
            new Intl.DateTimeFormat("root");
        }).toThrowWithMessage(RangeError, "root is not a structurally valid language tag");

        expect(() => {
            new Intl.DateTimeFormat("en-");
        }).toThrowWithMessage(RangeError, "en- is not a structurally valid language tag");

        expect(() => {
            new Intl.DateTimeFormat("Latn");
        }).toThrowWithMessage(RangeError, "Latn is not a structurally valid language tag");

        expect(() => {
            new Intl.DateTimeFormat("en-u-aa-U-aa");
        }).toThrowWithMessage(RangeError, "en-u-aa-U-aa is not a structurally valid language tag");
    });

    test("options is an invalid type", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("localeMatcher option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { localeMatcher: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option localeMatcher");
    });

    test("calendar option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { calendar: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option calendar");
    });

    test("numberingSystem option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { numberingSystem: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option numberingSystem");
    });

    test("hourCycle option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { hourCycle: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option hourCycle");
    });

    test("timeZone option is invalid", () => {
        ["hello!", "+1", "+1:02", "+01:02:03"].forEach(timeZone => {
            expect(() => {
                new Intl.DateTimeFormat("en", { timeZone: timeZone });
            }).toThrowWithMessage(
                RangeError,
                `${timeZone} is not a valid value for option timeZone`
            );
        });
    });

    test("era option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { era: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option era");

        expect(() => {
            new Intl.DateTimeFormat("en", { era: "narrow", dateStyle: "long" });
        }).toThrowWithMessage(
            TypeError,
            "Option era cannot be set when also providing dateStyle or timeStyle"
        );
    });

    test("year option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { year: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option year");

        expect(() => {
            new Intl.DateTimeFormat("en", { year: "numeric", dateStyle: "long" });
        }).toThrowWithMessage(
            TypeError,
            "Option year cannot be set when also providing dateStyle or timeStyle"
        );
    });

    test("month option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { month: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option month");

        expect(() => {
            new Intl.DateTimeFormat("en", { month: "numeric", dateStyle: "long" });
        }).toThrowWithMessage(
            TypeError,
            "Option month cannot be set when also providing dateStyle or timeStyle"
        );
    });

    test("weekday option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { weekday: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option weekday");

        expect(() => {
            new Intl.DateTimeFormat("en", { weekday: "narrow", dateStyle: "long" });
        }).toThrowWithMessage(
            TypeError,
            "Option weekday cannot be set when also providing dateStyle or timeStyle"
        );
    });

    test("day option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { day: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option day");

        expect(() => {
            new Intl.DateTimeFormat("en", { day: "numeric", dateStyle: "long" });
        }).toThrowWithMessage(
            TypeError,
            "Option day cannot be set when also providing dateStyle or timeStyle"
        );
    });

    test("dayPeriod option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { dayPeriod: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option dayPeriod");

        expect(() => {
            new Intl.DateTimeFormat("en", { dayPeriod: "narrow", dateStyle: "long" });
        }).toThrowWithMessage(
            TypeError,
            "Option dayPeriod cannot be set when also providing dateStyle or timeStyle"
        );
    });

    test("hour option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { hour: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option hour");

        expect(() => {
            new Intl.DateTimeFormat("en", { hour: "numeric", dateStyle: "long" });
        }).toThrowWithMessage(
            TypeError,
            "Option hour cannot be set when also providing dateStyle or timeStyle"
        );
    });

    test("minute option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { minute: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option minute");

        expect(() => {
            new Intl.DateTimeFormat("en", { minute: "numeric", dateStyle: "long" });
        }).toThrowWithMessage(
            TypeError,
            "Option minute cannot be set when also providing dateStyle or timeStyle"
        );
    });

    test("second option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { second: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option second");

        expect(() => {
            new Intl.DateTimeFormat("en", { second: "numeric", dateStyle: "long" });
        }).toThrowWithMessage(
            TypeError,
            "Option second cannot be set when also providing dateStyle or timeStyle"
        );
    });

    test("fractionalSecondDigits option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { fractionalSecondDigits: 1n });
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");

        expect(() => {
            new Intl.DateTimeFormat("en", { fractionalSecondDigits: "hello!" });
        }).toThrowWithMessage(RangeError, "Value NaN is NaN or is not between 1 and 3");

        expect(() => {
            new Intl.DateTimeFormat("en", { fractionalSecondDigits: 0 });
        }).toThrowWithMessage(RangeError, "Value 0 is NaN or is not between 1 and 3");

        expect(() => {
            new Intl.DateTimeFormat("en", { fractionalSecondDigits: 4 });
        }).toThrowWithMessage(RangeError, "Value 4 is NaN or is not between 1 and 3");

        expect(() => {
            new Intl.DateTimeFormat("en", { fractionalSecondDigits: 1, dateStyle: "long" });
        }).toThrowWithMessage(
            TypeError,
            "Option fractionalSecondDigits cannot be set when also providing dateStyle or timeStyle"
        );
    });

    test("timeZoneName option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { timeZoneName: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option timeZoneName");

        expect(() => {
            new Intl.DateTimeFormat("en", { timeZoneName: "short", dateStyle: "long" });
        }).toThrowWithMessage(
            TypeError,
            "Option timeZoneName cannot be set when also providing dateStyle or timeStyle"
        );
    });

    test("formatMatcher option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { formatMatcher: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option formatMatcher");
    });

    test("dateStyle option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { dateStyle: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option dateStyle");
    });

    test("timeStyle option is invalid", () => {
        expect(() => {
            new Intl.DateTimeFormat("en", { timeStyle: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option timeStyle");
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Intl.DateTimeFormat).toHaveLength(0);
    });

    test("all valid localeMatcher options", () => {
        ["lookup", "best fit"].forEach(localeMatcher => {
            expect(() => {
                new Intl.DateTimeFormat("en", { localeMatcher: localeMatcher });
            }).not.toThrow();
        });
    });

    test("valid calendar options", () => {
        ["generic", "gregory"].forEach(calendar => {
            expect(() => {
                new Intl.DateTimeFormat("en", { calendar: calendar });
            }).not.toThrow();
        });
    });

    test("valid numberingSystem options", () => {
        ["latn", "arab", "abc-def-ghi"].forEach(numberingSystem => {
            expect(() => {
                new Intl.DateTimeFormat("en", { numberingSystem: numberingSystem });
            }).not.toThrow();
        });
    });

    test("valid hour12 options", () => {
        [true, false].forEach(hour12 => {
            expect(() => {
                new Intl.DateTimeFormat("en", { hour12: hour12 });
            }).not.toThrow();
        });
    });

    test("all valid hourCycle options", () => {
        ["h11", "h12", "h23", "h24"].forEach(hourCycle => {
            expect(() => {
                new Intl.DateTimeFormat("en", { hourCycle: hourCycle });
            }).not.toThrow();
        });
    });

    test("valid timeZone options", () => {
        ["UTC", "EST", "+01:02", "-20:30", "+00:00"].forEach(timeZone => {
            expect(() => {
                new Intl.DateTimeFormat("en", { timeZone: timeZone });
            }).not.toThrow();
        });
    });

    test("all valid weekday options", () => {
        ["narrow", "short", "long"].forEach(weekday => {
            expect(() => {
                new Intl.DateTimeFormat("en", { weekday: weekday });
            }).not.toThrow();
        });
    });

    test("all valid era options", () => {
        ["narrow", "short", "long"].forEach(era => {
            expect(() => {
                new Intl.DateTimeFormat("en", { era: era });
            }).not.toThrow();
        });
    });

    test("all valid year options", () => {
        ["2-digit", "numeric"].forEach(year => {
            expect(() => {
                new Intl.DateTimeFormat("en", { year: year });
            }).not.toThrow();
        });
    });

    test("all valid month options", () => {
        ["2-digit", "numeric", "narrow", "short", "long"].forEach(month => {
            expect(() => {
                new Intl.DateTimeFormat("en", { month: month });
            }).not.toThrow();
        });
    });

    test("all valid day options", () => {
        ["2-digit", "numeric"].forEach(day => {
            expect(() => {
                new Intl.DateTimeFormat("en", { day: day });
            }).not.toThrow();
        });
    });

    test("all valid dayPeriod options", () => {
        ["narrow", "short", "long"].forEach(dayPeriod => {
            expect(() => {
                new Intl.DateTimeFormat("en", { dayPeriod: dayPeriod });
            }).not.toThrow();
        });
    });

    test("all valid hour options", () => {
        ["2-digit", "numeric"].forEach(hour => {
            expect(() => {
                new Intl.DateTimeFormat("en", { hour: hour });
            }).not.toThrow();
        });
    });

    test("all valid minute options", () => {
        ["2-digit", "numeric"].forEach(minute => {
            expect(() => {
                new Intl.DateTimeFormat("en", { minute: minute });
            }).not.toThrow();
        });
    });

    test("all valid second options", () => {
        ["2-digit", "numeric"].forEach(second => {
            expect(() => {
                new Intl.DateTimeFormat("en", { second: second });
            }).not.toThrow();
        });
    });

    test("all valid fractionalSecondDigits options", () => {
        [1, 2, 3].forEach(fractionalSecondDigits => {
            expect(() => {
                new Intl.DateTimeFormat("en", { fractionalSecondDigits: fractionalSecondDigits });
            }).not.toThrow();
        });
    });

    test("all valid timeZoneName options", () => {
        ["short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric"].forEach(
            timeZoneName => {
                expect(() => {
                    new Intl.DateTimeFormat("en", { timeZoneName: timeZoneName });
                }).not.toThrow();
            }
        );
    });

    test("all valid formatMatcher options", () => {
        ["basic", "best fit"].forEach(formatMatcher => {
            expect(() => {
                new Intl.DateTimeFormat("en", { formatMatcher: formatMatcher });
            }).not.toThrow();
        });
    });

    test("all valid dateStyle options", () => {
        ["full", "long", "medium", "short"].forEach(dateStyle => {
            expect(() => {
                new Intl.DateTimeFormat("en", { dateStyle: dateStyle });
            }).not.toThrow();
        });
    });

    test("all valid timeStyle options", () => {
        ["full", "long", "medium", "short"].forEach(timeStyle => {
            expect(() => {
                new Intl.DateTimeFormat("en", { timeStyle: timeStyle });
            }).not.toThrow();
        });
    });
});
