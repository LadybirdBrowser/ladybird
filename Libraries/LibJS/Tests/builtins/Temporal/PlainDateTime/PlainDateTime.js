describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Temporal.PlainDateTime();
        }).toThrowWithMessage(
            TypeError,
            "Temporal.PlainDateTime constructor must be called with 'new'"
        );
    });

    test("cannot pass Infinity", () => {
        for (let i = 0; i < 9; ++i) {
            const args = Array(9).fill(0);

            args[i] = Infinity;
            expect(() => {
                new Temporal.PlainDateTime(...args);
            }).toThrowWithMessage(RangeError, "Invalid plain date time");

            args[i] = -Infinity;
            expect(() => {
                new Temporal.PlainDateTime(...args);
            }).toThrowWithMessage(RangeError, "Invalid plain date time");
        }
    });

    test("cannot pass invalid ISO date or time", () => {
        // NOTE: The year max value is 3 more than in the polyfill, but they incorrectly seem to
        // use ISOYearMonthWithinLimits, which AFAICT isn't used for PlainDate{,Time} in the spec.
        // ¯\_(ツ)_/¯
        const badValues = [275764, 0, 0, 24, 60, 60, 1000, 1000, 1000];
        for (let i = 0; i < 9; ++i) {
            const args = [0, 1, 1, 0, 0, 0, 0, 0, 0];
            args[i] = badValues[i];
            expect(() => {
                new Temporal.PlainDateTime(...args);
            }).toThrowWithMessage(RangeError, "Invalid plain date time");
        }
    });
});

describe("normal behavior", () => {
    test("length is 3", () => {
        expect(Temporal.PlainDateTime).toHaveLength(3);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 22, 19, 46, 32, 123, 456, 789);
        expect(typeof plainDateTime).toBe("object");
        expect(plainDateTime).toBeInstanceOf(Temporal.PlainDateTime);
        expect(Object.getPrototypeOf(plainDateTime)).toBe(Temporal.PlainDateTime.prototype);
    });
});
