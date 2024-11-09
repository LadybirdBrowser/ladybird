describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDate.prototype.with).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(1970, 1, 1);
        const values = [
            [{ year: 2021 }, new Temporal.PlainDate(2021, 1, 1)],
            [{ year: 2021, month: 7 }, new Temporal.PlainDate(2021, 7, 1)],
            [{ year: 2021, month: 7, day: 6 }, new Temporal.PlainDate(2021, 7, 6)],
            [{ year: 2021, monthCode: "M07", day: 6 }, new Temporal.PlainDate(2021, 7, 6)],
        ];
        for (const [arg, expected] of values) {
            expect(plainDate.with(arg).equals(expected)).toBeTrue();
        }

        // Supplying the same values doesn't change the date, but still creates a new object
        const plainDateLike = { year: plainDate.year, month: plainDate.month, day: plainDate.day };
        expect(plainDate.with(plainDateLike)).not.toBe(plainDate);
        expect(plainDate.with(plainDateLike).equals(plainDate)).toBeTrue();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Temporal.PlainDate.prototype.with.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });

    test("argument must be an object", () => {
        expect(() => {
            new Temporal.PlainDate(1970, 1, 1).with("foo");
        }).toThrowWithMessage(TypeError, "foo is not an object");
        expect(() => {
            new Temporal.PlainDate(1970, 1, 1).with(42);
        }).toThrowWithMessage(TypeError, "42 is not an object");
    });

    test("argument must have one of 'day', 'month', 'monthCode', 'year'", () => {
        expect(() => {
            new Temporal.PlainDate(1970, 1, 1).with({});
        }).toThrowWithMessage(
            TypeError,
            "Object must have at least one of the following properties: day, month, monthCode, year"
        );
    });

    test("argument must not have 'calendar' or 'timeZone'", () => {
        expect(() => {
            new Temporal.PlainDate(1970, 1, 1).with({ calendar: {} });
        }).toThrowWithMessage(TypeError, "Object must not have a defined calendar property");
        expect(() => {
            new Temporal.PlainDate(1970, 1, 1).with({ timeZone: {} });
        }).toThrowWithMessage(TypeError, "Object must not have a defined timeZone property");
    });
});
