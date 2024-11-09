describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Now.plainDate).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const plainDate = Temporal.Now.plainDate(calendar);
        expect(plainDate).toBeInstanceOf(Temporal.PlainDate);
        expect(plainDate.calendar).toBe(calendar);
    });

    test("custom time zone", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const timeZone = {
            getOffsetNanosecondsFor() {
                return 86399999999999;
            },
        };
        const plainDate = Temporal.Now.plainDate(calendar, "UTC");
        const plainDateWithOffset = Temporal.Now.plainDate(calendar, timeZone);
        if (plainDate.dayOfYear === plainDate.daysInYear) {
            expect(plainDateWithOffset.year).toBe(plainDate.year + 1);
            expect(plainDateWithOffset.month).toBe(1);
            expect(plainDateWithOffset.day).toBe(1);
        } else {
            expect(plainDateWithOffset.year).toBe(plainDate.year);
            if (plainDate.day === plainDate.daysInMonth) {
                expect(plainDateWithOffset.month).toBe(plainDate.month + 1);
                expect(plainDateWithOffset.day).toBe(1);
            } else {
                expect(plainDateWithOffset.month).toBe(plainDate.month);
                expect(plainDateWithOffset.day).toBe(plainDate.day + 1);
            }
        }
    });

    test("cannot have a time zone with more than a day", () => {
        [86400000000000, -86400000000000, 86400000000001, 86400000000002].forEach(offset => {
            const calendar = new Temporal.Calendar("iso8601");
            const timeZone = {
                getOffsetNanosecondsFor() {
                    return offset;
                },
            };
            expect(() => Temporal.Now.plainDate(calendar, timeZone)).toThrowWithMessage(
                RangeError,
                "Invalid offset nanoseconds value, must be in range -86400 * 10^9 + 1 to 86400 * 10^9 - 1"
            );
        });
    });
});

describe("errors", () => {
    test("custom time zone doesn't have a getOffsetNanosecondsFor function", () => {
        expect(() => {
            Temporal.Now.plainDate({}, {});
        }).toThrowWithMessage(TypeError, "getOffsetNanosecondsFor is undefined");
    });
});
