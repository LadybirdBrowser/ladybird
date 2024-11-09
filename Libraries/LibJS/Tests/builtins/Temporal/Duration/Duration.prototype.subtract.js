describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Duration.prototype.subtract).toHaveLength(1);
    });

    function checkCommonResults(durationResult) {
        expect(durationResult.years).toBe(0);
        expect(durationResult.months).toBe(0);
        expect(durationResult.weeks).toBe(0);
        expect(durationResult.days).toBe(1);
        expect(durationResult.hours).toBe(1);
        expect(durationResult.minutes).toBe(1);
        expect(durationResult.seconds).toBe(1);
        expect(durationResult.milliseconds).toBe(1);
        expect(durationResult.microseconds).toBe(1);
        expect(durationResult.nanoseconds).toBe(1);
    }

    test("basic functionality", () => {
        const duration = new Temporal.Duration(0, 0, 0, 2, 2, 2, 2, 2, 2, 2);
        const oneDuration = new Temporal.Duration(0, 0, 0, 1, 1, 1, 1, 1, 1, 1);
        const durationResult = duration.subtract(oneDuration);

        checkCommonResults(durationResult);
    });

    test("from duration-like", () => {
        const duration = new Temporal.Duration(0, 0, 0, 2, 2, 2, 2, 2, 2, 2);
        const oneDuration = {
            days: 1,
            hours: 1,
            minutes: 1,
            seconds: 1,
            milliseconds: 1,
            microseconds: 1,
            nanoseconds: 1,
        };
        const durationResult = duration.subtract(oneDuration);

        checkCommonResults(durationResult);
    });

    test("from string", () => {
        const duration = new Temporal.Duration(0, 0, 0, 2, 2, 2, 2, 2, 2, 2);
        const oneDuration = "P1DT1H1M1.001001001S";
        const durationResult = duration.subtract(oneDuration);

        checkCommonResults(durationResult);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Duration object", () => {
        expect(() => {
            Temporal.Duration.prototype.subtract.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Duration");
    });

    test("relativeTo is required when duration has calendar units", () => {
        const yearDuration = new Temporal.Duration(1);
        const monthDuration = new Temporal.Duration(0, 1);
        const weekDuration = new Temporal.Duration(0, 0, 1);
        const durationToSubtract = { seconds: 1 };

        expect(() => {
            yearDuration.subtract(durationToSubtract);
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for balancing year, month or week"
        );

        expect(() => {
            monthDuration.subtract(durationToSubtract);
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for balancing year, month or week"
        );

        expect(() => {
            weekDuration.subtract(durationToSubtract);
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for balancing year, month or week"
        );
    });
});
