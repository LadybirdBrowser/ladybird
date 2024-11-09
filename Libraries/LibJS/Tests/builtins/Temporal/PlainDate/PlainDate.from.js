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

    test("PlainDateTime instance argument", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 26, 1, 2, 3);
        const createdPlainDate = Temporal.PlainDate.from(plainDateTime);
        expect(createdPlainDate.year).toBe(2021);
        expect(createdPlainDate.month).toBe(7);
        expect(createdPlainDate.day).toBe(26);
    });

    test("ZonedDateTime instance argument", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(1627318123456789000n, timeZone);
        const createdPlainDate = Temporal.PlainDate.from(zonedDateTime);
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
});

describe("errors", () => {
    test("custom time zone doesn't have a getOffsetNanosecondsFor function", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(0n, {});
        expect(() => {
            Temporal.PlainDate.from(zonedDateTime);
        }).toThrowWithMessage(TypeError, "getOffsetNanosecondsFor is undefined");
    });

    test("invalid date time string", () => {
        expect(() => {
            Temporal.PlainDate.from("foo");
        }).toThrowWithMessage(RangeError, "Invalid date time string 'foo'");
    });

    test("extended year must not be negative zero", () => {
        expect(() => {
            Temporal.PlainDate.from("-000000-01-01");
        }).toThrowWithMessage(RangeError, "Invalid date time string '-000000-01-01'");
        expect(() => {
            Temporal.PlainDate.from("−000000-01-01"); // U+2212
        }).toThrowWithMessage(RangeError, "Invalid date time string '−000000-01-01'");
    });
});
