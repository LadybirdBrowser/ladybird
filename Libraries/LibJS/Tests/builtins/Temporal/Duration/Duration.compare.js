describe("correct behavior", () => {
    test("length is 2", () => {
        expect(Temporal.Duration.compare).toHaveLength(2);
    });

    function checkCommonResults(duration1, duration2) {
        expect(Temporal.Duration.compare(duration1, duration1)).toBe(0);
        expect(Temporal.Duration.compare(duration2, duration2)).toBe(0);
        expect(Temporal.Duration.compare(duration1, duration2)).toBe(-1);
        expect(Temporal.Duration.compare(duration2, duration1)).toBe(1);
    }

    test("basic functionality", () => {
        const duration1 = new Temporal.Duration(0, 0, 0, 1);
        const duration2 = new Temporal.Duration(0, 0, 0, 2);
        checkCommonResults(duration1, duration2);
    });

    test("duration-like objects", () => {
        const duration1 = { years: 0, months: 0, weeks: 0, days: 1 };
        const duration2 = { years: 0, months: 0, weeks: 0, days: 2 };
        checkCommonResults(duration1, duration2);
    });

    test("duration strings", () => {
        const duration1 = "P1D";
        const duration2 = "P2D";
        checkCommonResults(duration1, duration2);
    });

    test("relative to plain date", () => {
        const oneMonth = new Temporal.Duration(0, 1);
        const thirtyDays = new Temporal.Duration(0, 0, 0, 30);

        let result = Temporal.Duration.compare(oneMonth, thirtyDays, {
            relativeTo: Temporal.PlainDate.from("2018-04-01"),
        });
        expect(result).toBe(0);

        result = Temporal.Duration.compare(oneMonth, thirtyDays, {
            relativeTo: Temporal.PlainDate.from("2018-03-01"),
        });
        expect(result).toBe(1);

        result = Temporal.Duration.compare(oneMonth, thirtyDays, {
            relativeTo: Temporal.PlainDate.from("2018-02-01"),
        });
        expect(result).toBe(-1);
    });
});

describe("errors", () => {
    test("invalid duration-like object", () => {
        expect(() => {
            Temporal.Duration.compare({});
        }).toThrowWithMessage(TypeError, "Invalid duration-like object");

        expect(() => {
            Temporal.Duration.compare({ years: 0, months: 0, weeks: 0, days: 1 }, {});
        }).toThrowWithMessage(TypeError, "Invalid duration-like object");
    });

    test("relativeTo is required for comparing calendar units (year, month, week)", () => {
        const duration1 = new Temporal.Duration(1);
        const duration2 = new Temporal.Duration(2);

        expect(() => {
            Temporal.Duration.compare(duration1, duration2);
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for comparing calendar units"
        );

        const duration3 = new Temporal.Duration(0, 3);
        const duration4 = new Temporal.Duration(0, 4);

        expect(() => {
            Temporal.Duration.compare(duration3, duration4);
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for comparing calendar units"
        );

        const duration5 = new Temporal.Duration(0, 0, 5);
        const duration6 = new Temporal.Duration(0, 0, 6);

        expect(() => {
            Temporal.Duration.compare(duration5, duration6);
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for comparing calendar units"
        );

        // Still throws if year/month/week of one the duration objects is non-zero.
        const duration7 = new Temporal.Duration(0, 0, 0, 7);
        const duration8 = new Temporal.Duration(0, 0, 8);

        expect(() => {
            Temporal.Duration.compare(duration7, duration8);
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for comparing calendar units"
        );

        const duration9 = new Temporal.Duration(0, 0, 9);
        const duration10 = new Temporal.Duration(0, 0, 0, 10);

        expect(() => {
            Temporal.Duration.compare(duration9, duration10);
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for comparing calendar units"
        );
    });
});
