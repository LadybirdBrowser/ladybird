describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.ZonedDateTime.prototype.toLocaleString).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(
            2021,
            11,
            3,
            1,
            33,
            5,
            100,
            200,
            300,
            "gregory"
        );
        const zonedDateTime = plainDateTime.toZonedDateTime("UTC");
        expect(zonedDateTime.toLocaleString()).toBe("11/3/2021, 1:33:5 AM UTC");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.toLocaleString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("Temporal object must have same calendar", () => {
        const plainDateTime = new Temporal.PlainDateTime(
            2021,
            11,
            3,
            1,
            33,
            5,
            100,
            200,
            300,
            "gregory"
        );
        const zonedDateTime = plainDateTime.toZonedDateTime("UTC");

        expect(() => {
            zonedDateTime.toLocaleString("en", { calendar: "iso8601" });
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.ZonedDateTime with calendar 'gregory' in locale with calendar 'iso8601'"
        );
    });
});
