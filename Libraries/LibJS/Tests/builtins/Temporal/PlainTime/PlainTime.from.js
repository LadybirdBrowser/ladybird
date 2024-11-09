describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainTime.from).toHaveLength(1);
    });

    test("PlainTime instance argument", () => {
        const plainTime = new Temporal.PlainTime(18, 45, 37, 1, 2, 3);
        const createdPlainTime = Temporal.PlainTime.from(plainTime);
        expect(createdPlainTime.hour).toBe(18);
        expect(createdPlainTime.minute).toBe(45);
        expect(createdPlainTime.second).toBe(37);
        expect(createdPlainTime.millisecond).toBe(1);
        expect(createdPlainTime.microsecond).toBe(2);
        expect(createdPlainTime.nanosecond).toBe(3);
    });

    test("PlainDateTime instance argument", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 8, 27, 18, 45, 37, 1, 2, 3);
        const createdPlainTime = Temporal.PlainTime.from(plainDateTime);
        expect(createdPlainTime.hour).toBe(18);
        expect(createdPlainTime.minute).toBe(45);
        expect(createdPlainTime.second).toBe(37);
        expect(createdPlainTime.millisecond).toBe(1);
        expect(createdPlainTime.microsecond).toBe(2);
        expect(createdPlainTime.nanosecond).toBe(3);
    });

    test("ZonedDateTime instance argument", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(1627318123456789000n, timeZone);
        const createdPlainTime = Temporal.PlainTime.from(zonedDateTime);
        expect(createdPlainTime.hour).toBe(16);
        expect(createdPlainTime.minute).toBe(48);
        expect(createdPlainTime.second).toBe(43);
        expect(createdPlainTime.millisecond).toBe(456);
        expect(createdPlainTime.microsecond).toBe(789);
        expect(createdPlainTime.nanosecond).toBe(0);
    });

    test("PlainTime string argument", () => {
        const createdPlainTime = Temporal.PlainTime.from("2021-08-27T18:44:11");
        expect(createdPlainTime.hour).toBe(18);
        expect(createdPlainTime.minute).toBe(44);
        expect(createdPlainTime.second).toBe(11);
        expect(createdPlainTime.millisecond).toBe(0);
        expect(createdPlainTime.microsecond).toBe(0);
        expect(createdPlainTime.nanosecond).toBe(0);
    });
});

describe("errors", () => {
    test("custom time zone doesn't have a getOffsetNanosecondsFor function", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(0n, {});
        expect(() => {
            Temporal.PlainTime.from(zonedDateTime);
        }).toThrowWithMessage(TypeError, "getOffsetNanosecondsFor is undefined");
    });

    test("string must not contain a UTC designator", () => {
        expect(() => {
            Temporal.PlainTime.from("2021-07-06T23:42:01Z");
        }).toThrowWithMessage(
            RangeError,
            "Invalid time string '2021-07-06T23:42:01Z': must not contain a UTC designator"
        );
    });

    test("extended year must not be negative zero", () => {
        expect(() => {
            Temporal.PlainTime.from("-000000-01-01T00:00:00");
        }).toThrowWithMessage(RangeError, "Invalid time string '-000000-01-01T00:00:00'");
        expect(() => {
            Temporal.PlainTime.from("−000000-01-01T00:00:00"); // U+2212
        }).toThrowWithMessage(RangeError, "Invalid time string '−000000-01-01T00:00:00'");
    });

    test("ambiguous string must contain a time designator", () => {
        const values = [
            // YYYY-MM or HHMM-UU
            "2021-12",
            // MMDD or HHMM
            "1214",
            "0229",
            "1130",
            // MM-DD or HH-UU
            "12-14",
            // YYYYMM or HHMMSS
            "202112",
        ];
        for (const value of values) {
            expect(() => {
                Temporal.PlainTime.from(value);
            }).toThrowWithMessage(RangeError, `Invalid time string '${value}'`);
            // Doesn't throw
            Temporal.PlainTime.from(`T${value}`);
        }
    });
});
