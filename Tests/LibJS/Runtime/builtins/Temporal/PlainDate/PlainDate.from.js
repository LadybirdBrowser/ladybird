describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDate.from).toHaveLength(1);
    });

    test("PlainDate instance argument", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 26);
        const createdPlainDate = Temporal.PlainDate.from(plainDate);
        expect(createdPlainDate.year).toBe(2021);
        expect(createdPlainDate.month).toBe(7);
        expect(createdPlainDate.day).toBe(26);
    });

    test("PlainDate string argument", () => {
        const createdPlainDate = Temporal.PlainDate.from("2021-07-26");
        expect(createdPlainDate.year).toBe(2021);
        expect(createdPlainDate.month).toBe(7);
        expect(createdPlainDate.day).toBe(26);
    });

    test("ZonedDateTime instance argument", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1627318123456789000n, "UTC");
        const createdPlainDate = Temporal.PlainDate.from(zonedDateTime);
        expect(createdPlainDate.year).toBe(2021);
        expect(createdPlainDate.month).toBe(7);
        expect(createdPlainDate.day).toBe(26);
    });

    test("non-ISO calendar from property bag", () => {
        const plainDate = Temporal.PlainDate.from({ year: 2024, month: 6, day: 15, calendar: "gregory" });
        expect(plainDate.calendarId).toBe("gregory");
        expect(plainDate.year).toBe(2024);
        expect(plainDate.month).toBe(6);
        expect(plainDate.day).toBe(15);
        expect(plainDate.era).toBe("ce");
        expect(plainDate.eraYear).toBe(2024);
    });

    test("non-ISO calendar from string with annotation", () => {
        const plainDate = Temporal.PlainDate.from("2024-06-15[u-ca=hebrew]");
        expect(plainDate.calendarId).toBe("hebrew");
        expect(plainDate.year).toBe(5784);
        expect(plainDate.monthCode).toBe("M09");
        expect(plainDate.day).toBe(9);
    });

    test("japanese calendar with era", () => {
        const plainDate = Temporal.PlainDate.from({ year: 2024, month: 6, day: 15, calendar: "japanese" });
        expect(plainDate.calendarId).toBe("japanese");
        expect(plainDate.era).toBe("reiwa");
        expect(plainDate.eraYear).toBe(6);
    });

    test("buddhist calendar year offset", () => {
        const plainDate = Temporal.PlainDate.from("2024-01-01[u-ca=buddhist]");
        expect(plainDate.calendarId).toBe("buddhist");
        expect(plainDate.year).toBe(2567);
    });

    test("hebrew calendar leap year has 13 months", () => {
        // 5784 is a Hebrew leap year.
        const plainDate = Temporal.PlainDate.from("2024-04-09[u-ca=hebrew]");
        expect(plainDate.calendarId).toBe("hebrew");
        expect(plainDate.year).toBe(5784);
        expect(plainDate.monthsInYear).toBe(13);
        expect(plainDate.inLeapYear).toBeTrue();
    });

    test("chinese calendar basic properties", () => {
        // 2024-02-10 is Chinese New Year (M01 day 1).
        // Chinese calendar uses the related ISO year.
        const plainDate = Temporal.PlainDate.from("2024-02-10[u-ca=chinese]");
        expect(plainDate.calendarId).toBe("chinese");
        expect(plainDate.year).toBe(2024);
        expect(plainDate.month).toBe(1);
        expect(plainDate.monthCode).toBe("M01");
        expect(plainDate.day).toBe(1);
    });

    test("chinese calendar leap month", () => {
        // 2023 has a Chinese leap month after M02.
        // 2023-03-22 falls in leap M02 (ordinal month 3).
        const plainDate = Temporal.PlainDate.from("2023-03-22[u-ca=chinese]");
        expect(plainDate.calendarId).toBe("chinese");
        expect(plainDate.monthCode).toBe("M02L");
        expect(plainDate.month).toBe(3);
    });

    test("overflow constrain clamps month", () => {
        // ISO calendar has 12 months, month 13 should be clamped to 12.
        const plainDate = Temporal.PlainDate.from(
            { year: 2024, month: 13, day: 1, calendar: "gregory" },
            { overflow: "constrain" }
        );
        expect(plainDate.month).toBe(12);
    });

    test("overflow constrain clamps day", () => {
        // February 2024 has 29 days (leap year), day 31 should be clamped to 29.
        const plainDate = Temporal.PlainDate.from(
            { year: 2024, month: 2, day: 31, calendar: "gregory" },
            { overflow: "constrain" }
        );
        expect(plainDate.day).toBe(29);
    });

    test("overflow reject throws on invalid month", () => {
        expect(() => {
            Temporal.PlainDate.from({ year: 2024, month: 13, day: 1, calendar: "gregory" }, { overflow: "reject" });
        }).toThrowWithMessage(RangeError, "month");
    });

    test("overflow reject throws on invalid day", () => {
        expect(() => {
            Temporal.PlainDate.from({ year: 2024, month: 2, day: 30, calendar: "gregory" }, { overflow: "reject" });
        }).toThrowWithMessage(RangeError, "day");
    });

    test("japanese era boundary: Heisei to Reiwa", () => {
        // April 30, 2019 is the last day of Heisei.
        const lastHeisei = Temporal.PlainDate.from("2019-04-30[u-ca=japanese]");
        expect(lastHeisei.era).toBe("heisei");
        expect(lastHeisei.eraYear).toBe(31);

        // May 1, 2019 is the first day of Reiwa.
        const firstReiwa = Temporal.PlainDate.from("2019-05-01[u-ca=japanese]");
        expect(firstReiwa.era).toBe("reiwa");
        expect(firstReiwa.eraYear).toBe(1);
    });
});

describe("errors", () => {
    test("missing fields", () => {
        expect(() => {
            Temporal.PlainDate.from({});
        }).toThrowWithMessage(TypeError, "Required property year is missing or undefined");
        expect(() => {
            Temporal.PlainDate.from({ year: 0 });
        }).toThrowWithMessage(TypeError, "Required property day is missing or undefined");
        expect(() => {
            Temporal.PlainDate.from({ month: 1 });
        }).toThrowWithMessage(TypeError, "Required property year is missing or undefined");
    });

    test("invalid date time string", () => {
        expect(() => {
            Temporal.PlainDate.from("foo");
        }).toThrowWithMessage(RangeError, "Invalid ISO date time");
    });

    test("extended year must not be negative zero", () => {
        expect(() => {
            Temporal.PlainDate.from("-000000-01-01");
        }).toThrowWithMessage(RangeError, "Invalid ISO date time");
        expect(() => {
            Temporal.PlainDate.from("−000000-01-01"); // U+2212
        }).toThrowWithMessage(RangeError, "Invalid ISO date time");
    });

    test("invalid calendar identifier", () => {
        expect(() => {
            Temporal.PlainDate.from({ year: 2024, month: 1, day: 1, calendar: "invalid" });
        }).toThrowWithMessage(RangeError, "Invalid calendar identifier 'invalid'");
    });

    test("invalid calendar annotation in string", () => {
        expect(() => {
            Temporal.PlainDate.from("2024-01-01[u-ca=invalid]");
        }).toThrowWithMessage(RangeError, "Invalid calendar identifier 'invalid'");
    });
});
