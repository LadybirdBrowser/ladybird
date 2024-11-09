describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.ZonedDateTime.prototype.toPlainDate).toHaveLength(0);
    });

    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone);
        const plainDate = zonedDateTime.toPlainDate();
        expect(plainDate).toBeInstanceOf(Temporal.PlainDate);
        expect(plainDate.year).toBe(2021);
        expect(plainDate.month).toBe(7);
        expect(plainDate.day).toBe(6);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.toPlainDate.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
