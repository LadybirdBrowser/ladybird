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
        expect(Temporal.Duration.prototype.abs).toHaveLength(0);
    });

    test("basic functionality", () => {
        let absoluteDuration;

        absoluteDuration = new Temporal.Duration(123).abs();
        expect(absoluteDuration.years).toBe(123);

        absoluteDuration = new Temporal.Duration(-123).abs();
        expect(absoluteDuration.years).toBe(123);
    });

    test("each property is made absolute", () => {
        let values;
        let duration;

        values = Array(DURATION_PROPERTIES.length).fill(-1);
        duration = new Temporal.Duration(...values).abs();
        for (const property of DURATION_PROPERTIES) {
            expect(duration[property]).toBe(1);
        }

        values = Array(DURATION_PROPERTIES.length).fill(1);
        duration = new Temporal.Duration(...values).abs();
        for (const property of DURATION_PROPERTIES) {
            expect(duration[property]).toBe(1);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Duration object", () => {
        expect(() => {
            Temporal.Duration.prototype.abs.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Duration");
    });
});
