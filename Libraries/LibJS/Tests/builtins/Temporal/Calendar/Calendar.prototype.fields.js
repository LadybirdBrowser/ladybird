describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Calendar.prototype.fields).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const array = [
            "year",
            "month",
            "monthCode",
            "day",
            "hour",
            "minute",
            "second",
            "millisecond",
            "microsecond",
            "nanosecond",
        ];
        const fields = calendar.fields(array);
        expect(fields).toEqual(array);
        expect(fields).not.toBe(array);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Calendar object", () => {
        expect(() => {
            Temporal.Calendar.prototype.fields.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Calendar");
    });

    test("iterator values must be strings", () => {
        const calendar = new Temporal.Calendar("iso8601");
        for (const value of [123, null, undefined, true, {}]) {
            expect(() => {
                calendar.fields([value]);
            }).toThrowWithMessage(TypeError, `Invalid calendar field ${value}, expected a string`);
        }
    });

    test("iterator values must be valid field names", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(() => {
            calendar.fields(["foo"]);
        }).toThrowWithMessage(RangeError, "Invalid calendar field 'foo'");
    });

    test("iterator values must not contain duplicates", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(() => {
            calendar.fields(["year", "month", "year", "month"]);
        }).toThrowWithMessage(RangeError, "Duplicate calendar field 'year'");
    });
});
