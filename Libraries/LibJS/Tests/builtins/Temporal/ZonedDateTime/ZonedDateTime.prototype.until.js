describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.prototype.until).toHaveLength(1);
    });

    test("basic functionality", () => {
        const values = [
            [0n, 0n, "PT0S"],
            [123456789n, 2345679011n, "PT2.222222222S"],
            [0n, 123456789n, "PT0.123456789S"],
            [123456789n, 0n, "-PT0.123456789S"],
            [0n, 123456789123456789n, "PT34293H33M9.123456789S"],
            [123456789123456789n, 0n, "-PT34293H33M9.123456789S"],
        ];
        const utc = new Temporal.TimeZone("UTC");
        for (const [arg, argOther, expected] of values) {
            const zonedDateTime = new Temporal.ZonedDateTime(arg, utc);
            const other = new Temporal.ZonedDateTime(argOther, utc);
            expect(zonedDateTime.until(other).toString()).toBe(expected);
        }
    });

    test("smallestUnit option", () => {
        const utc = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(0n, utc);
        const other = new Temporal.ZonedDateTime(34401906007008009n, utc);
        const values = [
            ["year", "P1Y"],
            ["month", "P13M"],
            ["week", "P56W"],
            ["day", "P398D"],
            ["hour", "PT9556H"],
            ["minute", "PT9556H5M"],
            ["second", "PT9556H5M6S"],
            ["millisecond", "PT9556H5M6.007S"],
            ["microsecond", "PT9556H5M6.007008S"],
            ["nanosecond", "PT9556H5M6.007008009S"],
        ];
        for (const [smallestUnit, expected] of values) {
            expect(zonedDateTime.until(other, { smallestUnit }).toString()).toBe(expected);
        }
    });

    test("largestUnit option", () => {
        const utc = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(0n, utc);
        const other = new Temporal.ZonedDateTime(34401906007008009n, utc);
        const values = [
            ["year", "P1Y1M2DT4H5M6.007008009S"],
            ["month", "P13M2DT4H5M6.007008009S"],
            ["week", "P56W6DT4H5M6.007008009S"],
            ["day", "P398DT4H5M6.007008009S"],
            ["hour", "PT9556H5M6.007008009S"],
            ["minute", "PT573365M6.007008009S"],
            ["second", "PT34401906.007008009S"],
            ["millisecond", "PT34401906.007008009S"],
            ["microsecond", "PT34401906.007008009S"],
            ["nanosecond", "PT34401906.007008008S"],
        ];
        for (const [largestUnit, expected] of values) {
            expect(zonedDateTime.until(other, { largestUnit }).toString()).toBe(expected);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.until.call("foo", {});
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("cannot compare dates from different calendars", () => {
        const calendarOne = {
            toString() {
                return "calendarOne";
            },
        };

        const calendarTwo = {
            toString() {
                return "calendarTwo";
            },
        };

        const utc = new Temporal.TimeZone("UTC");
        const zonedDateTimeOne = new Temporal.ZonedDateTime(0n, utc, calendarOne);
        const zonedDateTimeTwo = new Temporal.ZonedDateTime(0n, utc, calendarTwo);

        expect(() => {
            zonedDateTimeOne.until(zonedDateTimeTwo);
        }).toThrowWithMessage(RangeError, "Cannot compare dates from two different calendars");
    });

    test("cannot compare dates from different time zones", () => {
        const timeZoneOne = {
            toString() {
                return "timeZoneOne";
            },
        };

        const timeZoneTwo = {
            toString() {
                return "timeZoneTwo";
            },
        };

        const zonedDateTimeOne = new Temporal.ZonedDateTime(0n, timeZoneOne);
        const zonedDateTimeTwo = new Temporal.ZonedDateTime(0n, timeZoneTwo);

        expect(() => {
            zonedDateTimeOne.until(zonedDateTimeTwo, { largestUnit: "day" });
        }).toThrowWithMessage(RangeError, "Cannot compare dates from two different time zones");
    });
});

describe("rounding modes", () => {
    const earlier = new Temporal.ZonedDateTime(
        1546935756_123_456_789n /* 2019-01-08T08:22:36.123456789+00:00 */,
        "UTC"
    );
    const later = new Temporal.ZonedDateTime(
        1631018380_987_654_289n /* 2021-09-07T12:39:40.987654289+00:00 */,
        "UTC"
    );

    test("'ceil' rounding mode", () => {
        const expected = [
            ["years", "P3Y", "-P2Y"],
            ["months", "P32M", "-P31M"],
            ["weeks", "P140W", "-P139W"],
            ["days", "P974D", "-P973D"],
            ["hours", "PT23357H", "-PT23356H"],
            ["minutes", "PT23356H18M", "-PT23356H17M"],
            ["seconds", "PT23356H17M5S", "-PT23356H17M4S"],
            ["milliseconds", "PT23356H17M4.865S", "-PT23356H17M4.864S"],
            ["microseconds", "PT23356H17M4.864198S", "-PT23356H17M4.864197S"],
            ["nanoseconds", "PT23356H17M4.8641975S", "-PT23356H17M4.8641975S"],
        ];

        const roundingMode = "ceil";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'expand' rounding mode", () => {
        const expected = [
            ["years", "P3Y", "-P3Y"],
            ["months", "P32M", "-P32M"],
            ["weeks", "P140W", "-P140W"],
            ["days", "P974D", "-P974D"],
            ["hours", "PT23357H", "-PT23357H"],
            ["minutes", "PT23356H18M", "-PT23356H18M"],
            ["seconds", "PT23356H17M5S", "-PT23356H17M5S"],
            ["milliseconds", "PT23356H17M4.865S", "-PT23356H17M4.865S"],
            ["microseconds", "PT23356H17M4.864198S", "-PT23356H17M4.864198S"],
            ["nanoseconds", "PT23356H17M4.8641975S", "-PT23356H17M4.8641975S"],
        ];

        const roundingMode = "expand";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'floor' rounding mode", () => {
        const expected = [
            ["years", "P2Y", "-P3Y"],
            ["months", "P31M", "-P32M"],
            ["weeks", "P139W", "-P140W"],
            ["days", "P973D", "-P974D"],
            ["hours", "PT23356H", "-PT23357H"],
            ["minutes", "PT23356H17M", "-PT23356H18M"],
            ["seconds", "PT23356H17M4S", "-PT23356H17M5S"],
            ["milliseconds", "PT23356H17M4.864S", "-PT23356H17M4.865S"],
            ["microseconds", "PT23356H17M4.864197S", "-PT23356H17M4.864198S"],
            ["nanoseconds", "PT23356H17M4.8641975S", "-PT23356H17M4.8641975S"],
        ];

        const roundingMode = "floor";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'trunc' rounding mode", () => {
        const expected = [
            ["years", "P2Y", "-P2Y"],
            ["months", "P31M", "-P31M"],
            ["weeks", "P139W", "-P139W"],
            ["days", "P973D", "-P973D"],
            ["hours", "PT23356H", "-PT23356H"],
            ["minutes", "PT23356H17M", "-PT23356H17M"],
            ["seconds", "PT23356H17M4S", "-PT23356H17M4S"],
            ["milliseconds", "PT23356H17M4.864S", "-PT23356H17M4.864S"],
            ["microseconds", "PT23356H17M4.864197S", "-PT23356H17M4.864197S"],
            ["nanoseconds", "PT23356H17M4.8641975S", "-PT23356H17M4.8641975S"],
        ];

        const roundingMode = "trunc";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'halfCeil' rounding mode", () => {
        const expected = [
            ["years", "P3Y", "-P3Y"],
            ["months", "P32M", "-P32M"],
            ["weeks", "P139W", "-P139W"],
            ["days", "P973D", "-P973D"],
            ["hours", "PT23356H", "-PT23356H"],
            ["minutes", "PT23356H17M", "-PT23356H17M"],
            ["seconds", "PT23356H17M5S", "-PT23356H17M5S"],
            ["milliseconds", "PT23356H17M4.864S", "-PT23356H17M4.864S"],
            ["microseconds", "PT23356H17M4.864198S", "-PT23356H17M4.864197S"],
            ["nanoseconds", "PT23356H17M4.8641975S", "-PT23356H17M4.8641975S"],
        ];

        const roundingMode = "halfCeil";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'halfEven' rounding mode", () => {
        const expected = [
            ["years", "P3Y", "-P3Y"],
            ["months", "P32M", "-P32M"],
            ["weeks", "P139W", "-P139W"],
            ["days", "P973D", "-P973D"],
            ["hours", "PT23356H", "-PT23356H"],
            ["minutes", "PT23356H17M", "-PT23356H17M"],
            ["seconds", "PT23356H17M5S", "-PT23356H17M5S"],
            ["milliseconds", "PT23356H17M4.864S", "-PT23356H17M4.864S"],
            ["microseconds", "PT23356H17M4.864198S", "-PT23356H17M4.864198S"],
            ["nanoseconds", "PT23356H17M4.8641975S", "-PT23356H17M4.8641975S"],
        ];

        const roundingMode = "halfEven";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'halfTrunc' rounding mode", () => {
        const expected = [
            ["years", "P3Y", "-P3Y"],
            ["months", "P32M", "-P32M"],
            ["weeks", "P139W", "-P139W"],
            ["days", "P973D", "-P973D"],
            ["hours", "PT23356H", "-PT23356H"],
            ["minutes", "PT23356H17M", "-PT23356H17M"],
            ["seconds", "PT23356H17M5S", "-PT23356H17M5S"],
            ["milliseconds", "PT23356H17M4.864S", "-PT23356H17M4.864S"],
            ["microseconds", "PT23356H17M4.864197S", "-PT23356H17M4.864197S"],
            ["nanoseconds", "PT23356H17M4.8641975S", "-PT23356H17M4.8641975S"],
        ];

        const roundingMode = "halfTrunc";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'halfExpand' rounding mode", () => {
        const expected = [
            ["years", "P3Y", "-P3Y"],
            ["months", "P32M", "-P32M"],
            ["weeks", "P139W", "-P139W"],
            ["days", "P973D", "-P973D"],
            ["hours", "PT23356H", "-PT23356H"],
            ["minutes", "PT23356H17M", "-PT23356H17M"],
            ["seconds", "PT23356H17M5S", "-PT23356H17M5S"],
            ["milliseconds", "PT23356H17M4.864S", "-PT23356H17M4.864S"],
            ["microseconds", "PT23356H17M4.864198S", "-PT23356H17M4.864198S"],
            ["nanoseconds", "PT23356H17M4.8641975S", "-PT23356H17M4.8641975S"],
        ];

        const roundingMode = "halfExpand";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'halfFloor' rounding mode", () => {
        const expected = [
            ["years", "P3Y", "-P3Y"],
            ["months", "P32M", "-P32M"],
            ["weeks", "P139W", "-P139W"],
            ["days", "P973D", "-P973D"],
            ["hours", "PT23356H", "-PT23356H"],
            ["minutes", "PT23356H17M", "-PT23356H17M"],
            ["seconds", "PT23356H17M5S", "-PT23356H17M5S"],
            ["milliseconds", "PT23356H17M4.864S", "-PT23356H17M4.864S"],
            ["microseconds", "PT23356H17M4.864197S", "-PT23356H17M4.864198S"],
            ["nanoseconds", "PT23356H17M4.8641975S", "-PT23356H17M4.8641975S"],
        ];

        const roundingMode = "halfFloor";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });
});
