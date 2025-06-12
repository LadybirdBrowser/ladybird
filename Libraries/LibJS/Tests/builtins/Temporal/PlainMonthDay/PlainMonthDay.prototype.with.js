describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainMonthDay.prototype.with).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainMonthDay = new Temporal.PlainMonthDay(1, 1);
        const values = [
            [{ monthCode: "M07" }, new Temporal.PlainMonthDay(7, 1)],
            [{ monthCode: "M07", day: 6 }, new Temporal.PlainMonthDay(7, 6)],
            [{ year: 0, month: 7, day: 6 }, new Temporal.PlainMonthDay(7, 6)],
        ];
        for (const [arg, expected] of values) {
            expect(plainMonthDay.with(arg).equals(expected)).toBeTrue();
        }

        // Supplying the same values doesn't change the month/day, but still creates a new object
        const plainMonthDayLike = {
            month: plainMonthDay.month,
            day: plainMonthDay.day,
        };
        expect(plainMonthDay.with(plainMonthDayLike)).not.toBe(plainMonthDay);
        expect(plainMonthDay.with(plainMonthDayLike).equals(plainMonthDay)).toBeTrue();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainMonthDay object", () => {
        expect(() => {
            Temporal.PlainMonthDay.prototype.with.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainMonthDay");
    });

    test("argument must be an object", () => {
        expect(() => {
            new Temporal.PlainMonthDay(1, 1).with("foo");
        }).toThrowWithMessage(TypeError, "Object must be a partial Temporal object");
        expect(() => {
            new Temporal.PlainMonthDay(1, 1).with(42);
        }).toThrowWithMessage(TypeError, "Object must be a partial Temporal object");
    });

    test("argument must have at least one Temporal property", () => {
        expect(() => {
            new Temporal.PlainMonthDay(1, 1).with({});
        }).toThrowWithMessage(TypeError, "Object must be a partial Temporal object");
    });

    test("argument must not have 'calendar' or 'timeZone'", () => {
        expect(() => {
            new Temporal.PlainMonthDay(1, 1).with({ calendar: {} });
        }).toThrowWithMessage(TypeError, "Object must be a partial Temporal object");
        expect(() => {
            new Temporal.PlainMonthDay(1, 1).with({ timeZone: {} });
        }).toThrowWithMessage(TypeError, "Object must be a partial Temporal object");
    });
});
