const DURATION_PROPERTIES = [
    "years",
    "months",
    "weeks",
    "days",
    "hours",
    "minutes",
    "seconds",
    "milliseconds",
    "microseconds",
    "nanoseconds",
];

describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.Duration.prototype.negated).toHaveLength(0);
    });

    test("basic functionality", () => {
        const negativeDuration = new Temporal.Duration(123).negated();
        expect(negativeDuration.years).toBe(-123);

        const positiveDuration = new Temporal.Duration(-123).negated();
        expect(positiveDuration.years).toBe(123);
    });

    test("each property is negated", () => {
        const values = Array(DURATION_PROPERTIES.length).fill(1);
        const duration = new Temporal.Duration(...values).negated();
        for (const property of DURATION_PROPERTIES) {
            expect(duration[property]).toBe(-1);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Duration object", () => {
        expect(() => {
            Temporal.Duration.prototype.negated.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Duration");
    });
});
