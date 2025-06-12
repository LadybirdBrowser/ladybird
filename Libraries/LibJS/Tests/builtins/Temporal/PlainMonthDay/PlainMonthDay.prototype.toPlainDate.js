describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainMonthDay.prototype.toPlainDate).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainMonthDay = new Temporal.PlainMonthDay(7, 6);
        const plainDate = plainMonthDay.toPlainDate({ year: 2021 });
        expect(plainDate.equals(new Temporal.PlainDate(2021, 7, 6))).toBeTrue();
    });
});

describe("errors", () => {
    test("argument must be an object", () => {
        const plainMonthDay = new Temporal.PlainMonthDay(7, 6);
        expect(() => {
            plainMonthDay.toPlainDate(42);
        }).toThrowWithMessage(TypeError, "42 is not an object");
    });

    test("year field is required", () => {
        const plainMonthDay = new Temporal.PlainMonthDay(7, 6);
        expect(() => {
            plainMonthDay.toPlainDate({});
        }).toThrowWithMessage(TypeError, "Required property year is missing or undefined");
    });
});
