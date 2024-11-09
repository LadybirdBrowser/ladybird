describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Temporal.PlainMonthDay();
        }).toThrowWithMessage(
            TypeError,
            "Temporal.PlainMonthDay constructor must be called with 'new'"
        );
    });

    test("cannot pass Infinity", () => {
        expect(() => {
            new Temporal.PlainMonthDay(Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain month day");
        expect(() => {
            new Temporal.PlainMonthDay(1, Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain month day");
        expect(() => {
            new Temporal.PlainMonthDay(1, 1, {}, Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain month day");
        expect(() => {
            new Temporal.PlainMonthDay(-Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain month day");
        expect(() => {
            new Temporal.PlainMonthDay(1, -Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain month day");
        expect(() => {
            new Temporal.PlainMonthDay(1, 1, {}, -Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain month day");
    });

    test("cannot pass invalid ISO month/day", () => {
        expect(() => {
            new Temporal.PlainMonthDay(0, 1);
        }).toThrowWithMessage(RangeError, "Invalid plain month day");
        expect(() => {
            new Temporal.PlainMonthDay(1, 0);
        }).toThrowWithMessage(RangeError, "Invalid plain month day");
    });

    test("not within iso date time limit", () => {
        expect(() => {
            new Temporal.PlainMonthDay(9, 30, "iso8601", 999_999_999_999_999);
        }).toThrowWithMessage(RangeError, "Invalid plain month day");
    });
});

describe("normal behavior", () => {
    test("length is 2", () => {
        expect(Temporal.PlainMonthDay).toHaveLength(2);
    });

    test("basic functionality", () => {
        const plainMonthDay = new Temporal.PlainMonthDay(7, 6);
        expect(typeof plainMonthDay).toBe("object");
        expect(plainMonthDay).toBeInstanceOf(Temporal.PlainMonthDay);
        expect(Object.getPrototypeOf(plainMonthDay)).toBe(Temporal.PlainMonthDay.prototype);
    });

    test("default reference year is 1972", () => {
        const plainMonthDay = new Temporal.PlainMonthDay(7, 6);
        const fields = plainMonthDay.getISOFields();
        expect(fields.isoYear).toBe(1972);
    });
});
