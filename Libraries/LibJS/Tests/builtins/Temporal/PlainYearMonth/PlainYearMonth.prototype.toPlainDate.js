describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainYearMonth.prototype.toPlainDate).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        const plainDate = plainYearMonth.toPlainDate({ day: 6 });
        expect(plainDate.equals(new Temporal.PlainDate(2021, 7, 6))).toBeTrue();
    });
});

describe("errors", () => {
    test("argument must be an object", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        expect(() => {
            plainYearMonth.toPlainDate(42);
        }).toThrowWithMessage(TypeError, "42 is not an object");
    });

    test("day field is required", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        expect(() => {
            plainYearMonth.toPlainDate({});
        }).toThrowWithMessage(TypeError, "Required property day is missing or undefined");
    });
});
