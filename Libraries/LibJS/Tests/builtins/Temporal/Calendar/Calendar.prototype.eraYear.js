describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Calendar.prototype.eraYear).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        const calendar = new Temporal.Calendar("iso8601");
        expect(calendar.eraYear(plainDate)).toBeUndefined();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Calendar object", () => {
        expect(() => {
            Temporal.Calendar.prototype.eraYear.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Calendar");
    });

    test("argument must be date-like", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(() => {
            calendar.eraYear({});
        }).toThrowWithMessage(TypeError, "Required property day is missing or undefined");
    });
});
