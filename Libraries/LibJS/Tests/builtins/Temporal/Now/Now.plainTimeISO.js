describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.Now.plainTimeISO).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainTime = Temporal.Now.plainTimeISO();
        expect(plainTime).toBeInstanceOf(Temporal.PlainTime);
        expect(plainTime.calendar.id).toBe("iso8601");
    });

    test("custom time zone", () => {
        const timeZone = {
            getOffsetNanosecondsFor() {
                return 86399999999999;
            },
        };
        const plainTime = Temporal.Now.plainTimeISO("UTC");
        const plainTimeWithOffset = Temporal.Now.plainTimeISO(timeZone);
        // FIXME: Compare these in a sensible way
    });

    test("cannot have a time zone with more than a day", () => {
        [86400000000000, -86400000000000, 86400000000001, 86400000000002].forEach(offset => {
            const timeZone = {
                getOffsetNanosecondsFor() {
                    return offset;
                },
            };
            expect(() => Temporal.Now.plainTimeISO(timeZone)).toThrowWithMessage(
                RangeError,
                "Invalid offset nanoseconds value, must be in range -86400 * 10^9 + 1 to 86400 * 10^9 - 1"
            );
        });
    });
});

describe("errors", () => {
    test("custom time zone doesn't have a getOffsetNanosecondsFor function", () => {
        expect(() => {
            Temporal.Now.plainTimeISO({});
        }).toThrowWithMessage(TypeError, "getOffsetNanosecondsFor is undefined");
    });
});
