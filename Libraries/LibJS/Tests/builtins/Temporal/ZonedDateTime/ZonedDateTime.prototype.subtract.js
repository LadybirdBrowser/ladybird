describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.prototype.subtract).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 12, 0, 22, 30, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const dayDuration = new Temporal.Duration(0, 0, 0, 1);
        const result = zonedDateTime.subtract(dayDuration);

        expect(zonedDateTime.day).toBe(12);
        expect(zonedDateTime.epochNanoseconds).toBe(1636676550100200300n);

        expect(result.day).toBe(11);
        expect(result.epochNanoseconds).toBe(1636590150100200300n);
    });

    test("duration-like object", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 12, 0, 22, 30, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const dayDuration = { days: 1 };
        const result = zonedDateTime.subtract(dayDuration);

        expect(zonedDateTime.day).toBe(12);
        expect(zonedDateTime.epochNanoseconds).toBe(1636676550100200300n);

        expect(result.day).toBe(11);
        expect(result.epochNanoseconds).toBe(1636590150100200300n);
    });

    test("duration string", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 12, 0, 22, 30, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const dayDuration = "P1D";
        const result = zonedDateTime.subtract(dayDuration);

        expect(zonedDateTime.day).toBe(12);
        expect(zonedDateTime.epochNanoseconds).toBe(1636676550100200300n);

        expect(result.day).toBe(11);
        expect(result.epochNanoseconds).toBe(1636590150100200300n);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.subtract.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("invalid duration-like object", () => {
        expect(() => {
            new Temporal.ZonedDateTime(1n, {}).subtract({});
        }).toThrowWithMessage(TypeError, "Invalid duration-like object");
    });
});
