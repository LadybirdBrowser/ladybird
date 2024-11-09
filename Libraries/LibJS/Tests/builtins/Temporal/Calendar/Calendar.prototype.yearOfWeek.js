describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Calendar.prototype.yearOfWeek).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const date = new Temporal.PlainDate(2023, 1, 1);
        expect(calendar.yearOfWeek(date)).toBe(2022);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Calendar object", () => {
        expect(() => {
            Temporal.Calendar.prototype.yearOfWeek.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Calendar");
    });
});
