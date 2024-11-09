describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainYearMonth.prototype.with).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(1970, 1);
        const values = [
            [{ year: 2021 }, new Temporal.PlainYearMonth(2021, 1)],
            [{ year: 2021, month: 7 }, new Temporal.PlainYearMonth(2021, 7)],
            [{ year: 2021, monthCode: "M07" }, new Temporal.PlainYearMonth(2021, 7)],
        ];
        for (const [arg, expected] of values) {
            expect(plainYearMonth.with(arg).equals(expected)).toBeTrue();
        }

        // Supplying the same values doesn't change the year/month, but still creates a new object
        const plainYearMonthLike = { year: plainYearMonth.year, month: plainYearMonth.month };
        expect(plainYearMonth.with(plainYearMonthLike)).not.toBe(plainYearMonth);
        expect(plainYearMonth.with(plainYearMonthLike).equals(plainYearMonth)).toBeTrue();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Temporal.PlainYearMonth.prototype.with.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });

    test("argument must be an object", () => {
        expect(() => {
            new Temporal.PlainYearMonth(1970, 1).with("foo");
        }).toThrowWithMessage(TypeError, "foo is not an object");
        expect(() => {
            new Temporal.PlainYearMonth(1970, 1).with(42);
        }).toThrowWithMessage(TypeError, "42 is not an object");
    });

    test("argument must have one of 'month', 'monthCode', 'year'", () => {
        expect(() => {
            new Temporal.PlainYearMonth(1970, 1).with({});
        }).toThrowWithMessage(
            TypeError,
            "Object must have at least one of the following properties: month, monthCode, year"
        );
    });

    test("argument must not have 'calendar' or 'timeZone'", () => {
        expect(() => {
            new Temporal.PlainYearMonth(1970, 1).with({ calendar: {} });
        }).toThrowWithMessage(TypeError, "Object must not have a defined calendar property");
        expect(() => {
            new Temporal.PlainYearMonth(1970, 1).with({ timeZone: {} });
        }).toThrowWithMessage(TypeError, "Object must not have a defined timeZone property");
    });
});
