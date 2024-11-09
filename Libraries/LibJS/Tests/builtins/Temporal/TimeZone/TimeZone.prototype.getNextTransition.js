describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.TimeZone.prototype.getNextTransition).toHaveLength(1);
    });

    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const instant = new Temporal.Instant(0n);
        expect(timeZone.getNextTransition(instant)).toBeNull();
    });

    test("custom offset", () => {
        const timeZone = new Temporal.TimeZone("+01:30");
        const instant = new Temporal.Instant(0n);
        expect(timeZone.getNextTransition(instant)).toBeNull();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.TimeZone object", () => {
        expect(() => {
            Temporal.TimeZone.prototype.getNextTransition.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.TimeZone");
    });
});
