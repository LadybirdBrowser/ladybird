describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.Instant.prototype.toString).toHaveLength(0);
    });

    test("basic functionality", () => {
        const instant = new Temporal.Instant(1625614921123456789n);
        expect(instant.toString()).toBe("2021-07-06T23:42:01.123456789Z");
    });

    test("timeZone option", () => {
        const instant = new Temporal.Instant(1625614921123456789n);
        const options = { timeZone: new Temporal.TimeZone("+01:30") };
        expect(instant.toString(options)).toBe("2021-07-07T01:12:01.123456789+01:30");
    });

    test("fractionalSecondDigits option", () => {
        const instant = new Temporal.Instant(1625614921123456000n);
        const values = [
            ["auto", "2021-07-06T23:42:01.123456Z"],
            [0, "2021-07-06T23:42:01Z"],
            [1, "2021-07-06T23:42:01.1Z"],
            [2, "2021-07-06T23:42:01.12Z"],
            [3, "2021-07-06T23:42:01.123Z"],
            [4, "2021-07-06T23:42:01.1234Z"],
            [5, "2021-07-06T23:42:01.12345Z"],
            [6, "2021-07-06T23:42:01.123456Z"],
            [7, "2021-07-06T23:42:01.1234560Z"],
            [8, "2021-07-06T23:42:01.12345600Z"],
            [9, "2021-07-06T23:42:01.123456000Z"],
        ];
        for (const [fractionalSecondDigits, expected] of values) {
            const options = { fractionalSecondDigits };
            expect(instant.toString(options)).toBe(expected);
        }

        // Ignored when smallestUnit is given
        expect(instant.toString({ smallestUnit: "minute", fractionalSecondDigits: 9 })).toBe(
            "2021-07-06T23:42Z"
        );
    });

    test("smallestUnit option", () => {
        const instant = new Temporal.Instant(1625614921123456789n);
        const values = [
            ["minute", "2021-07-06T23:42Z"],
            ["second", "2021-07-06T23:42:01Z"],
            ["millisecond", "2021-07-06T23:42:01.123Z"],
            ["microsecond", "2021-07-06T23:42:01.123456Z"],
            ["nanosecond", "2021-07-06T23:42:01.123456789Z"],
        ];
        for (const [smallestUnit, expected] of values) {
            const options = { smallestUnit };
            expect(instant.toString(options)).toBe(expected);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Instant object", () => {
        expect(() => {
            Temporal.Instant.prototype.toString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Instant");
    });

    test("custom time zone doesn't have a getOffsetNanosecondsFor function", () => {
        const instant = new Temporal.Instant(0n);
        expect(() => {
            instant.toString({ timeZone: {} });
        }).toThrowWithMessage(TypeError, "getOffsetNanosecondsFor is undefined");
    });
});
