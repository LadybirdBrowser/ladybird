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
});
