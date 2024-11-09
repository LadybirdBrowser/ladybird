describe("correct behavior", () => {
    test("length is 2", () => {
        expect(Temporal.Calendar.prototype.dateUntil).toHaveLength(2);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const one = new Temporal.PlainDate(2021, 7, 6);
        const two = new Temporal.PlainDate(2021, 10, 10);

        const oneToTwo = calendar.dateUntil(one, two);
        expect(oneToTwo.years).toBe(0);
        expect(oneToTwo.months).toBe(0);
        expect(oneToTwo.weeks).toBe(0);
        expect(oneToTwo.days).toBe(96);
        expect(oneToTwo.hours).toBe(0);
        expect(oneToTwo.minutes).toBe(0);
        expect(oneToTwo.seconds).toBe(0);
        expect(oneToTwo.milliseconds).toBe(0);
        expect(oneToTwo.microseconds).toBe(0);
        expect(oneToTwo.nanoseconds).toBe(0);

        const twoToOne = calendar.dateUntil(two, one);
        expect(twoToOne.years).toBe(0);
        expect(twoToOne.months).toBe(0);
        expect(twoToOne.weeks).toBe(0);
        expect(twoToOne.days).toBe(-96);
        expect(twoToOne.hours).toBe(0);
        expect(twoToOne.minutes).toBe(0);
        expect(twoToOne.seconds).toBe(0);
        expect(twoToOne.milliseconds).toBe(0);
        expect(twoToOne.microseconds).toBe(0);
        expect(twoToOne.nanoseconds).toBe(0);
    });

    test("largestUnit option", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const one = new Temporal.PlainDate(1970, 1, 1);
        const two = new Temporal.PlainDate(2021, 7, 6);

        const values = [
            ["years", 51, 6, 0, 5],
            ["months", 0, 618, 0, 5],
            ["weeks", 0, 0, 2687, 5],
            ["days", 0, 0, 0, 18814],
        ];
        for (const [largestUnit, years, months, weeks, days] of values) {
            const duration = calendar.dateUntil(one, two, { largestUnit });
            expect(duration.years).toBe(years);
            expect(duration.months).toBe(months);
            expect(duration.weeks).toBe(weeks);
            expect(duration.days).toBe(days);
            expect(duration.hours).toBe(0);
            expect(duration.minutes).toBe(0);
            expect(duration.seconds).toBe(0);
            expect(duration.milliseconds).toBe(0);
            expect(duration.microseconds).toBe(0);
            expect(duration.nanoseconds).toBe(0);
        }
    });
});

describe("errors", () => {
    test("forbidden largestUnit option values", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const one = new Temporal.PlainDate(1970, 1, 1);
        const two = new Temporal.PlainDate(2021, 7, 6);

        const values = ["hour", "minute", "second", "millisecond", "microsecond", "nanosecond"];
        for (const largestUnit of values) {
            expect(() => {
                calendar.dateUntil(one, two, { largestUnit });
            }).toThrowWithMessage(
                RangeError,
                `${largestUnit} is not a valid value for option largestUnit`
            );
        }
    });
});
