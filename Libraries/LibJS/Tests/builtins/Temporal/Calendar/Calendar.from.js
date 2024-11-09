describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Calendar.from).toHaveLength(1);
    });

    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const plainDate = new Temporal.PlainDate(1970, 1, 1);
        const plainTime = new Temporal.PlainTime();
        const plainDateTime = new Temporal.PlainDateTime(1970, 1, 1);
        const plainMonthDay = new Temporal.PlainMonthDay(1, 1);
        const plainYearMonth = new Temporal.PlainYearMonth(1970, 1);
        const zonedDateTime = new Temporal.ZonedDateTime(0n, timeZone);
        const calendarLike = {};
        const withCalendarLike = { calendar: {} };
        expect(Temporal.Calendar.from(plainDate)).toBe(plainDate.calendar);
        expect(Temporal.Calendar.from(plainTime)).toBe(plainTime.calendar);
        expect(Temporal.Calendar.from(plainDateTime)).toBe(plainDateTime.calendar);
        expect(Temporal.Calendar.from(plainMonthDay)).toBe(plainMonthDay.calendar);
        expect(Temporal.Calendar.from(plainYearMonth)).toBe(plainYearMonth.calendar);
        expect(Temporal.Calendar.from(zonedDateTime)).toBe(zonedDateTime.calendar);
        expect(Temporal.Calendar.from(calendarLike)).toBe(calendarLike);
        expect(Temporal.Calendar.from(withCalendarLike)).toBe(withCalendarLike.calendar);
        expect(Temporal.Calendar.from("iso8601").id).toBe("iso8601");
        expect(Temporal.Calendar.from("2021-07-06[u-ca=iso8601]").id).toBe("iso8601");
    });

    test("ToTemporalCalendar fast path returns if it is passed a Temporal.Calendar instance", () => {
        // This is obseravble via there being no property lookups (avoiding a "calendar" property lookup in this case)
        let madeObservableHasPropertyLookup = false;
        class Calendar extends Temporal.Calendar {
            constructor() {
                super("iso8601");
            }

            get calendar() {
                madeObservableHasPropertyLookup = true;
                return this;
            }
        }
        const calendar = new Calendar();
        Temporal.Calendar.from(calendar);
        expect(madeObservableHasPropertyLookup).toBeFalse();
    });
});

describe("errors", () => {
    test("Calendar from TimeZone", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        expect(() => {
            Temporal.Calendar.from(timeZone);
        }).toThrowWithMessage(
            RangeError,
            "Got unexpected TimeZone object in conversion to Calendar"
        );
    });

    test("yyyy-mm and mm-dd strings can only use the iso8601 calendar", () => {
        // FIXME: The error message doesn't really indicate this is the case.
        const values = [
            "02-10[u-ca=iso8602]",
            "02-10[u-ca=SerenityOS]",
            "2023-02[u-ca=iso8602]",
            "2023-02[u-ca=SerenityOS]",
        ];

        for (const value of values) {
            expect(() => {
                Temporal.Calendar.from(value);
            }).toThrowWithMessage(RangeError, `Invalid calendar string '${value}'`);
        }
    });
});
