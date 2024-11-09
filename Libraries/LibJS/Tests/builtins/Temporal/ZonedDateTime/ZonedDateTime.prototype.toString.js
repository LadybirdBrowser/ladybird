describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.ZonedDateTime.prototype.toString).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 1, 33, 5, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        expect(zonedDateTime.toString()).toBe("2021-11-03T01:33:05.1002003+00:00[UTC]");
    });

    test("negative epoch nanoseconds", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(-999_999_999n, timeZone);
        expect(zonedDateTime.toString()).toBe("1969-12-31T23:59:59.000000001+00:00[UTC]");
    });

    test("fractionalSecondDigits option", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 1, 33, 5, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const values = [
            ["auto", "2021-11-03T01:33:05.1002003+00:00[UTC]"],
            [0, "2021-11-03T01:33:05+00:00[UTC]"],
            [1, "2021-11-03T01:33:05.1+00:00[UTC]"],
            [2, "2021-11-03T01:33:05.10+00:00[UTC]"],
            [3, "2021-11-03T01:33:05.100+00:00[UTC]"],
            [4, "2021-11-03T01:33:05.1002+00:00[UTC]"],
            [5, "2021-11-03T01:33:05.10020+00:00[UTC]"],
            [6, "2021-11-03T01:33:05.100200+00:00[UTC]"],
            [7, "2021-11-03T01:33:05.1002003+00:00[UTC]"],
            [8, "2021-11-03T01:33:05.10020030+00:00[UTC]"],
            [9, "2021-11-03T01:33:05.100200300+00:00[UTC]"],
        ];

        for (const [fractionalSecondDigits, expected] of values) {
            const options = { fractionalSecondDigits };
            expect(zonedDateTime.toString(options)).toBe(expected);
        }

        // Ignored when smallestUnit is given
        expect(zonedDateTime.toString({ smallestUnit: "minute", fractionalSecondDigits: 9 })).toBe(
            "2021-11-03T01:33+00:00[UTC]"
        );
    });

    test("smallestUnit option", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 1, 33, 5, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const values = [
            ["minute", "2021-11-03T01:33+00:00[UTC]"],
            ["second", "2021-11-03T01:33:05+00:00[UTC]"],
            ["millisecond", "2021-11-03T01:33:05.100+00:00[UTC]"],
            ["microsecond", "2021-11-03T01:33:05.100200+00:00[UTC]"],
            ["nanosecond", "2021-11-03T01:33:05.100200300+00:00[UTC]"],
        ];

        for (const [smallestUnit, expected] of values) {
            const singularOptions = { smallestUnit };
            const pluralOptions = { smallestUnit: `${smallestUnit}s` };
            expect(zonedDateTime.toString(singularOptions)).toBe(expected);
            expect(zonedDateTime.toString(pluralOptions)).toBe(expected);
        }
    });

    test("timeZoneName option", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 1, 33, 5, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const values = [
            ["auto", "2021-11-03T01:33:05.1002003+00:00[UTC]"],
            ["never", "2021-11-03T01:33:05.1002003+00:00"],
            ["critical", "2021-11-03T01:33:05.1002003+00:00[!UTC]"],
        ];

        for (const [timeZoneName, expected] of values) {
            const options = { timeZoneName };
            expect(zonedDateTime.toString(options)).toBe(expected);
        }
    });

    test("offset option", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 1, 33, 5, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const values = [
            ["auto", "2021-11-03T01:33:05.1002003+00:00[UTC]"],
            ["never", "2021-11-03T01:33:05.1002003[UTC]"],
        ];

        for (const [offset, expected] of values) {
            const options = { offset };
            expect(zonedDateTime.toString(options)).toBe(expected);
        }
    });

    test("doesn't call ToString on calendar if calenderName option is 'never'", () => {
        let calledToString = false;
        const calendar = {
            toString() {
                calledToString = true;
                return "nocall";
            },
        };

        const plainDateTime = new Temporal.PlainDateTime(
            2022,
            8,
            8,
            14,
            38,
            40,
            100,
            200,
            300,
            calendar
        );
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);

        const options = {
            calendarName: "never",
        };
        expect(zonedDateTime.toString(options)).toBe("2022-08-08T14:38:40.1002003+00:00[UTC]");
        expect(calledToString).toBeFalse();
    });

    test("calendarName option", () => {
        const plainDateTime = new Temporal.PlainDateTime(2022, 11, 2, 19, 4, 35, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const values = [
            ["auto", "2022-11-02T19:04:35.1002003+00:00[UTC]"],
            ["always", "2022-11-02T19:04:35.1002003+00:00[UTC][u-ca=iso8601]"],
            ["never", "2022-11-02T19:04:35.1002003+00:00[UTC]"],
            ["critical", "2022-11-02T19:04:35.1002003+00:00[UTC][!u-ca=iso8601]"],
        ];

        for (const [calendarName, expected] of values) {
            const options = { calendarName };
            expect(zonedDateTime.toString(options)).toBe(expected);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.toString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("custom time zone doesn't have a getOffsetNanosecondsFor function", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(0n, {});
        expect(() => {
            zonedDateTime.toString();
        }).toThrowWithMessage(TypeError, "getOffsetNanosecondsFor is undefined");
    });

    test("calendarName option must be one of 'auto', 'always', 'never', 'critical'", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(0n, "UTC");
        expect(() => {
            zonedDateTime.toString({ calendarName: "foo" });
        }).toThrowWithMessage(RangeError, "foo is not a valid value for option calendarName");
    });
});
