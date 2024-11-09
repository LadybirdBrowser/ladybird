describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Intl.DurationFormat();
        }).toThrowWithMessage(
            TypeError,
            "Intl.DurationFormat constructor must be called with 'new'"
        );
    });

    test("structurally invalid tag", () => {
        expect(() => {
            new Intl.DurationFormat("root");
        }).toThrowWithMessage(RangeError, "root is not a structurally valid language tag");

        expect(() => {
            new Intl.DurationFormat("en-");
        }).toThrowWithMessage(RangeError, "en- is not a structurally valid language tag");

        expect(() => {
            new Intl.DurationFormat("Latn");
        }).toThrowWithMessage(RangeError, "Latn is not a structurally valid language tag");

        expect(() => {
            new Intl.DurationFormat("en-u-aa-U-aa");
        }).toThrowWithMessage(RangeError, "en-u-aa-U-aa is not a structurally valid language tag");
    });

    test("options is an invalid type", () => {
        expect(() => {
            new Intl.DurationFormat("en", null);
        }).toThrowWithMessage(TypeError, "Options is not an object");
    });

    test("localeMatcher option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { localeMatcher: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option localeMatcher");
    });

    test("style option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { style: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option style");
    });

    test("years option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { years: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option years");
    });

    test("yearsDisplay option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { yearsDisplay: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option yearsDisplay");
    });

    test("months option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { months: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option months");
    });

    test("monthsDisplay option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { monthsDisplay: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option monthsDisplay");
    });

    test("weeks option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { weeks: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option weeks");
    });

    test("weeksDisplay option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { weeksDisplay: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option weeksDisplay");
    });

    test("days option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { days: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option days");
    });

    test("daysDisplay option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { daysDisplay: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option daysDisplay");
    });

    test("hours option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { hours: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option hours");
    });

    test("hoursDisplay option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { hoursDisplay: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option hoursDisplay");
    });

    test("minutes option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { minutes: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option minutes");
    });

    test("minutesDisplay option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { minutesDisplay: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option minutesDisplay");
    });

    test("seconds option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { seconds: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option seconds");
    });

    test("secondsDisplay option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { secondsDisplay: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option secondsDisplay");
    });

    test("milliseconds option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { milliseconds: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option milliseconds");
    });

    test("millisecondsDisplay option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { millisecondsDisplay: "hello!" });
        }).toThrowWithMessage(
            RangeError,
            "hello! is not a valid value for option millisecondsDisplay"
        );
    });

    test("microseconds option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { microseconds: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option microseconds");
    });

    test("microsecondsDisplay option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { microsecondsDisplay: "hello!" });
        }).toThrowWithMessage(
            RangeError,
            "hello! is not a valid value for option microsecondsDisplay"
        );
    });

    test("nanoseconds option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { nanoseconds: "hello!" });
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option nanoseconds");
    });

    test("nanosecondsDisplay option is invalid", () => {
        expect(() => {
            new Intl.DurationFormat("en", { nanosecondsDisplay: "hello!" });
        }).toThrowWithMessage(
            RangeError,
            "hello! is not a valid value for option nanosecondsDisplay"
        );
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Intl.DurationFormat).toHaveLength(0);
    });

    test("all valid localeMatcher options", () => {
        ["lookup", "best fit"].forEach(localeMatcher => {
            expect(() => {
                new Intl.DurationFormat("en", { localeMatcher: localeMatcher });
            }).not.toThrow();
        });
    });

    test("all valid style options", () => {
        ["long", "short", "narrow", "digital"].forEach(style => {
            expect(() => {
                new Intl.DurationFormat("en", { style: style });
            }).not.toThrow();
        });
    });

    test("all valid years options", () => {
        ["long", "short", "narrow"].forEach(years => {
            expect(() => {
                new Intl.DurationFormat("en", { years: years });
            }).not.toThrow();
        });
    });

    test("all valid yearsDisplay options", () => {
        ["always", "auto"].forEach(yearsDisplay => {
            expect(() => {
                new Intl.DurationFormat("en", { yearsDisplay: yearsDisplay });
            }).not.toThrow();
        });
    });

    test("all valid months options", () => {
        ["long", "short", "narrow"].forEach(months => {
            expect(() => {
                new Intl.DurationFormat("en", { months: months });
            }).not.toThrow();
        });
    });

    test("all valid monthsDisplay options", () => {
        ["always", "auto"].forEach(monthsDisplay => {
            expect(() => {
                new Intl.DurationFormat("en", { monthsDisplay: monthsDisplay });
            }).not.toThrow();
        });
    });

    test("all valid weeks options", () => {
        ["long", "short", "narrow"].forEach(weeks => {
            expect(() => {
                new Intl.DurationFormat("en", { weeks: weeks });
            }).not.toThrow();
        });
    });

    test("all valid weeksDisplay options", () => {
        ["always", "auto"].forEach(weeksDisplay => {
            expect(() => {
                new Intl.DurationFormat("en", { weeksDisplay: weeksDisplay });
            }).not.toThrow();
        });
    });

    test("all valid days options", () => {
        ["long", "short", "narrow"].forEach(days => {
            expect(() => {
                new Intl.DurationFormat("en", { days: days });
            }).not.toThrow();
        });
    });

    test("all valid daysDisplay options", () => {
        ["always", "auto"].forEach(daysDisplay => {
            expect(() => {
                new Intl.DurationFormat("en", { daysDisplay: daysDisplay });
            }).not.toThrow();
        });
    });

    test("all valid hours options", () => {
        ["long", "short", "narrow"].forEach(hours => {
            expect(() => {
                new Intl.DurationFormat("en", { hours: hours });
            }).not.toThrow();
        });
        ["numeric", "2-digit"].forEach(seconds => {
            expect(() => {
                new Intl.DurationFormat("en", { hours: seconds });
            }).not.toThrow();

            expect(() => {
                new Intl.DurationFormat("en", { style: "digital", hours: seconds });
            }).not.toThrow();
        });
    });

    test("all valid hoursDisplay options", () => {
        ["always", "auto"].forEach(hoursDisplay => {
            expect(() => {
                new Intl.DurationFormat("en", { hoursDisplay: hoursDisplay });
            }).not.toThrow();
        });
    });

    test("all valid minutes options", () => {
        ["long", "short", "narrow"].forEach(minutes => {
            expect(() => {
                new Intl.DurationFormat("en", { minutes: minutes });
            }).not.toThrow();
        });
        ["numeric", "2-digit"].forEach(seconds => {
            expect(() => {
                new Intl.DurationFormat("en", { minutes: seconds });
            }).not.toThrow();

            expect(() => {
                new Intl.DurationFormat("en", { style: "digital", minutes: seconds });
            }).not.toThrow();
        });
    });

    test("all valid minutesDisplay options", () => {
        ["always", "auto"].forEach(minutesDisplay => {
            expect(() => {
                new Intl.DurationFormat("en", { minutesDisplay: minutesDisplay });
            }).not.toThrow();
        });
    });

    test("all valid seconds options", () => {
        ["long", "short", "narrow"].forEach(seconds => {
            expect(() => {
                new Intl.DurationFormat("en", { seconds: seconds });
            }).not.toThrow();
        });
        ["numeric", "2-digit"].forEach(seconds => {
            expect(() => {
                new Intl.DurationFormat("en", { seconds: seconds });
            }).not.toThrow();

            expect(() => {
                new Intl.DurationFormat("en", { style: "digital", seconds: seconds });
            }).not.toThrow();
        });
    });

    test("all valid secondsDisplay options", () => {
        ["always", "auto"].forEach(secondsDisplay => {
            expect(() => {
                new Intl.DurationFormat("en", { secondsDisplay: secondsDisplay });
            }).not.toThrow();
        });
    });

    test("all valid milliseconds options", () => {
        ["long", "short", "narrow"].forEach(milliseconds => {
            expect(() => {
                new Intl.DurationFormat("en", { milliseconds: milliseconds });
            }).not.toThrow();
        });

        expect(() => {
            new Intl.DurationFormat("en", { milliseconds: "numeric" });
        }).not.toThrow();

        expect(() => {
            new Intl.DurationFormat("en", { style: "digital", milliseconds: "numeric" });
        }).not.toThrow();
    });

    test("all valid millisecondsDisplay options", () => {
        ["always", "auto"].forEach(millisecondsDisplay => {
            expect(() => {
                new Intl.DurationFormat("en", { millisecondsDisplay: millisecondsDisplay });
            }).not.toThrow();
        });
    });

    test("all valid microseconds options", () => {
        ["long", "short", "narrow"].forEach(microseconds => {
            expect(() => {
                new Intl.DurationFormat("en", { microseconds: microseconds });
            }).not.toThrow();
        });

        expect(() => {
            new Intl.DurationFormat("en", { microseconds: "numeric" });
        }).not.toThrow();

        expect(() => {
            new Intl.DurationFormat("en", { style: "digital", microseconds: "numeric" });
        }).not.toThrow();
    });

    test("all valid microsecondsDisplay options", () => {
        ["always", "auto"].forEach(microsecondsDisplay => {
            expect(() => {
                new Intl.DurationFormat("en", { microsecondsDisplay: microsecondsDisplay });
            }).not.toThrow();
        });
    });

    test("all valid nanoseconds options", () => {
        ["long", "short", "narrow", "numeric"].forEach(nanoseconds => {
            expect(() => {
                new Intl.DurationFormat("en", { nanoseconds: nanoseconds });
            }).not.toThrow();
        });

        expect(() => {
            new Intl.DurationFormat("en", { nanoseconds: "numeric" });
        }).not.toThrow();

        expect(() => {
            new Intl.DurationFormat("en", { style: "digital", nanoseconds: "numeric" });
        }).not.toThrow();
    });

    test("all valid nanosecondsDisplay options", () => {
        ["always", "auto"].forEach(nanosecondsDisplay => {
            expect(() => {
                new Intl.DurationFormat("en", { nanosecondsDisplay: nanosecondsDisplay });
            }).not.toThrow();
        });
    });

    test("all valid fractionalDigits options", () => {
        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9].forEach(fractionalDigits => {
            expect(() => {
                new Intl.DurationFormat("en", { fractionalDigits: fractionalDigits });
            }).not.toThrow();
        });
    });
});
