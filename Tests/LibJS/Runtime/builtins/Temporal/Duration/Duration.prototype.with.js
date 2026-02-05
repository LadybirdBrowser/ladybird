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
    test("length is 1", () => {
        expect(Temporal.Duration.prototype.with).toHaveLength(1);
    });

    test("basic functionality", () => {
        const duration = new Temporal.Duration(1, 2, 3).with({ years: 4, foo: 5, weeks: 6 });
        expect(duration.years).toBe(4);
        expect(duration.months).toBe(2);
        expect(duration.weeks).toBe(6);
    });

    test("each property is looked up from the object", () => {
        for (const property of DURATION_PROPERTIES) {
            const duration = new Temporal.Duration().with({ [property]: 1 });
            expect(duration[property]).toBe(1);
        }
    });

    test("each property is coerced to number", () => {
        for (const property of DURATION_PROPERTIES) {
            const duration = new Temporal.Duration().with({ [property]: "1" });
            expect(duration[property]).toBe(1);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Duration object", () => {
        expect(() => {
            Temporal.Duration.prototype.with.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Duration");
    });

    test("argument is not an object", () => {
        expect(() => {
            new Temporal.Duration().with("foo");
        }).toThrowWithMessage(TypeError, "foo is not an object");
        expect(() => {
            new Temporal.Duration().with(42);
        }).toThrowWithMessage(TypeError, "42 is not an object");
    });

    test("argument is an invalid duration-like object", () => {
        expect(() => {
            new Temporal.Duration().with({});
        }).toThrowWithMessage(TypeError, "Invalid duration-like object");
        expect(() => {
            new Temporal.Duration().with({ foo: 1, bar: 2 });
        }).toThrowWithMessage(TypeError, "Invalid duration-like object");
    });

    test("error when coercing property to number", () => {
        for (const property of DURATION_PROPERTIES) {
            expect(() => {
                new Temporal.Duration().with({
                    [property]: {
                        valueOf() {
                            throw new Error();
                        },
                    },
                });
            }).toThrow(Error);
        }
    });

    test("invalid duration value", () => {
        for (const property of DURATION_PROPERTIES) {
            // NOTE: NaN does *not* throw a RangeError anymore - which is questionable, IMO - as of:
            // https://github.com/tc39/proposal-temporal/commit/8c854507a52efbc6e9eb2642f0f928df38e5c021
            for (const value of [1.23, Infinity]) {
                expect(() => {
                    new Temporal.Duration().with({ [property]: value });
                }).toThrowWithMessage(
                    RangeError,
                    `Invalid value for duration property '${property}': must be an integer, got ${value}`
                );
            }
        }
    });
});
