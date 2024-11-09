describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Duration.prototype.add).toHaveLength(1);
    });

    function checkCommonResults(durationResult) {
        expect(durationResult.years).toBe(0);
        expect(durationResult.months).toBe(0);
        expect(durationResult.weeks).toBe(0);
        expect(durationResult.days).toBe(3);
        expect(durationResult.hours).toBe(3);
        expect(durationResult.minutes).toBe(3);
        expect(durationResult.seconds).toBe(3);
        expect(durationResult.milliseconds).toBe(3);
        expect(durationResult.microseconds).toBe(3);
        expect(durationResult.nanoseconds).toBe(3);
    }

    test("basic functionality", () => {
        const duration = new Temporal.Duration(0, 0, 0, 2, 2, 2, 2, 2, 2, 2);
        const oneDuration = new Temporal.Duration(0, 0, 0, 1, 1, 1, 1, 1, 1, 1);
        const durationResult = duration.add(oneDuration);

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
        const durationResult = duration.add(oneDuration);

        checkCommonResults(durationResult);
    });

    test("from string", () => {
        const duration = new Temporal.Duration(0, 0, 0, 2, 2, 2, 2, 2, 2, 2);
        const oneDuration = "P1DT1H1M1.001001001S";
        const durationResult = duration.add(oneDuration);

        checkCommonResults(durationResult);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Duration object", () => {
        expect(() => {
            Temporal.Duration.prototype.add.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Duration");
    });

    test("relativeTo is required when duration has calendar units", () => {
        const yearDuration = new Temporal.Duration(1);
        const monthDuration = new Temporal.Duration(0, 1);
        const weekDuration = new Temporal.Duration(0, 0, 1);
        const durationToAdd = { seconds: 1 };

        expect(() => {
            yearDuration.add(durationToAdd);
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for balancing year, month or week"
        );

        expect(() => {
            monthDuration.add(durationToAdd);
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for balancing year, month or week"
        );

        expect(() => {
            weekDuration.add(durationToAdd);
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for balancing year, month or week"
        );
    });
});
