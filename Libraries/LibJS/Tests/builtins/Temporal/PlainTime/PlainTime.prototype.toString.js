describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainTime.prototype.toString).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainTime = new Temporal.PlainTime(18, 14, 47, 123, 456, 789);
        expect(plainTime.toString()).toBe("18:14:47.123456789");
    });

    test("fractionalSecondDigits option", () => {
        const plainTime = new Temporal.PlainTime(18, 14, 47, 123, 456);
        const values = [
            ["auto", "18:14:47.123456"],
            [0, "18:14:47"],
            [1, "18:14:47.1"],
            [2, "18:14:47.12"],
            [3, "18:14:47.123"],
            [4, "18:14:47.1234"],
            [5, "18:14:47.12345"],
            [6, "18:14:47.123456"],
            [7, "18:14:47.1234560"],
            [8, "18:14:47.12345600"],
            [9, "18:14:47.123456000"],
        ];
        for (const [fractionalSecondDigits, expected] of values) {
            const options = { fractionalSecondDigits };
            expect(plainTime.toString(options)).toBe(expected);
        }

        // Ignored when smallestUnit is given
        expect(plainTime.toString({ smallestUnit: "minute", fractionalSecondDigits: 9 })).toBe(
            "18:14"
        );
    });

    test("smallestUnit option", () => {
        const plainTime = new Temporal.PlainTime(18, 14, 47, 123, 456, 789);
        const values = [
            ["minute", "18:14"],
            ["second", "18:14:47"],
            ["millisecond", "18:14:47.123"],
            ["microsecond", "18:14:47.123456"],
            ["nanosecond", "18:14:47.123456789"],
        ];
        for (const [smallestUnit, expected] of values) {
            const options = { smallestUnit };
            expect(plainTime.toString(options)).toBe(expected);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainTime object", () => {
        expect(() => {
            Temporal.PlainTime.prototype.toString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainTime");
    });
});
