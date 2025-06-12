describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Temporal.PlainYearMonth();
        }).toThrowWithMessage(
            TypeError,
            "Temporal.PlainYearMonth constructor must be called with 'new'"
        );
    });

    test("cannot pass Infinity", () => {
        expect(() => {
            new Temporal.PlainYearMonth(Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain year month");
        expect(() => {
            new Temporal.PlainYearMonth(0, Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain year month");
        expect(() => {
            new Temporal.PlainYearMonth(0, 1, undefined, Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain year month");
        expect(() => {
            new Temporal.PlainYearMonth(-Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain year month");
        expect(() => {
            new Temporal.PlainYearMonth(0, -Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain year month");
        expect(() => {
            new Temporal.PlainYearMonth(0, 1, undefined, -Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain year month");
    });

    test("cannot pass invalid ISO month/day", () => {
        expect(() => {
            new Temporal.PlainYearMonth(0, 0);
        }).toThrowWithMessage(RangeError, "Invalid plain year month");
        expect(() => {
            new Temporal.PlainYearMonth(0, 1, undefined, 0);
        }).toThrowWithMessage(RangeError, "Invalid plain year month");
    });
});

describe("normal behavior", () => {
    test("length is 2", () => {
        expect(Temporal.PlainYearMonth).toHaveLength(2);
    });

    test("basic functionality", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        expect(typeof plainYearMonth).toBe("object");
        expect(plainYearMonth).toBeInstanceOf(Temporal.PlainYearMonth);
        expect(Object.getPrototypeOf(plainYearMonth)).toBe(Temporal.PlainYearMonth.prototype);
    });

    test("default reference day is 1", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        const fields = plainYearMonth.toString({ calendarName: "always" });
        const day = fields.split("-")[2].split("[")[0];
        expect(day).toBe("01");
    });
});
