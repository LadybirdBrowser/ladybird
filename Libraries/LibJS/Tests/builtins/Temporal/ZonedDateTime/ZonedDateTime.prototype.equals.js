describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.prototype.equals).toHaveLength(1);
    });

    test("basic functionality", () => {
        const zonedDateTimeOne = new Temporal.ZonedDateTime(1n, new Temporal.TimeZone("UTC"));
        const zonedDateTimeTwo = new Temporal.ZonedDateTime(2n, new Temporal.TimeZone("UTC"));

        expect(zonedDateTimeOne.equals(zonedDateTimeOne)).toBeTrue();
        expect(zonedDateTimeTwo.equals(zonedDateTimeTwo)).toBeTrue();
        expect(zonedDateTimeOne.equals(zonedDateTimeTwo)).toBeFalse();
        expect(zonedDateTimeTwo.equals(zonedDateTimeOne)).toBeFalse();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.equals.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
