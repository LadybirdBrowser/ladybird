describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Temporal.PlainDate();
        }).toThrowWithMessage(
            TypeError,
            "Temporal.PlainDate constructor must be called with 'new'"
        );
    });

    test("cannot pass Infinity", () => {
        expect(() => {
            new Temporal.PlainDate(Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain date");
        expect(() => {
            new Temporal.PlainDate(0, Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain date");
        expect(() => {
            new Temporal.PlainDate(0, 0, Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain date");
        expect(() => {
            new Temporal.PlainDate(-Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain date");
        expect(() => {
            new Temporal.PlainDate(0, -Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain date");
        expect(() => {
            new Temporal.PlainDate(0, 0, -Infinity);
        }).toThrowWithMessage(RangeError, "Invalid plain date");
    });

    test("cannot pass invalid ISO month/day", () => {
        expect(() => {
            new Temporal.PlainDate(0, 0, 1);
        }).toThrowWithMessage(RangeError, "Invalid plain date");
        expect(() => {
            new Temporal.PlainDate(0, 1, 0);
        }).toThrowWithMessage(RangeError, "Invalid plain date");
    });
});

describe("normal behavior", () => {
    test("length is 3", () => {
        expect(Temporal.PlainDate).toHaveLength(3);
    });

    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 19);
        expect(typeof plainDate).toBe("object");
        expect(plainDate).toBeInstanceOf(Temporal.PlainDate);
        expect(Object.getPrototypeOf(plainDate)).toBe(Temporal.PlainDate.prototype);
    });
});
