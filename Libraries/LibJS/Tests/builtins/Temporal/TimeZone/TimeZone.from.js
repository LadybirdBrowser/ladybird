describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Temporal.TimeZone.from).toHaveLength(1);
    });

    test("basic functionality", () => {
        // From object
        const timeZone = new Temporal.TimeZone("UTC");
        const timeZoneLike = {};
        const zonedDateTimeLike = { timeZone: {} };
        expect(Temporal.TimeZone.from(timeZone)).toBe(timeZone);
        expect(Temporal.TimeZone.from(timeZoneLike)).toBe(timeZoneLike);
        expect(Temporal.TimeZone.from(zonedDateTimeLike)).toBe(zonedDateTimeLike.timeZone);

        // From string
        const values = [
            ["UTC", "UTC"],
            ["GMT", "UTC"],
            ["Etc/UTC", "UTC"],
            ["Etc/GMT", "UTC"],
            ["Etc/GMT0", "UTC"], // IANA legacy name
            ["Etc/GMT+0", "UTC"], // IANA legacy name
            ["Etc/GMT-0", "UTC"], // IANA legacy name
            ["Etc/GMT+6", "Etc/GMT+6"],
            ["Etc/GMT-6", "Etc/GMT-6"],
            ["Etc/GMT+12", "Etc/GMT+12"],
            ["Etc/GMT-12", "Etc/GMT-12"],
            ["Europe/London", "Europe/London"],
            ["Australia/Canberra", "Australia/Sydney"],
            ["1970-01-01T00:00:00+01", "+01:00"],
            ["1970-01-01T00:00:00.000000000+01", "+01:00"],
            ["1970-01-01T00:00:00.000000000+01:00:00", "+01:00"],
        ];
        for (const [arg, expected] of values) {
            expect(Temporal.TimeZone.from(arg).id).toBe(expected);
        }
    });

    test("ToTemporalTimeZone fast path returns if it is passed a Temporal.TimeZone instance", () => {
        // This is obseravble via there being no property lookups (avoiding a "timeZone" property lookup in this case)
        let madeObservableHasPropertyLookup = false;
        class TimeZone extends Temporal.TimeZone {
            constructor() {
                super("UTC");
            }

            get timeZone() {
                madeObservableHasPropertyLookup = true;
                return this;
            }
        }
        const timeZone = new TimeZone();
        Temporal.TimeZone.from(timeZone);
        expect(madeObservableHasPropertyLookup).toBeFalse();
    });
});

describe("errors", () => {
    test("TimeZone from Calendar", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(() => {
            Temporal.TimeZone.from(calendar);
        }).toThrowWithMessage(
            RangeError,
            "Got unexpected Calendar object in conversion to TimeZone"
        );
    });

    test("invalid time zone strings", () => {
        const values = [
            "1970-01-01+01",
            "1970-01-01+01[-12:34]",
            "1970-01-01+12:34",
            "1970-01-01+12:34:56",
            "1970-01-01+12:34:56.789",
            "1970-01-01+12:34:56.789[-01:00]",
            "1970-01-01-12:34",
            "1970-01-01-12:34:56",
            "1970-01-01-12:34:56.789",
            "1970-01-01-12:34:56.789[+01:00]",
        ];

        for (const value of values) {
            expect(() => {
                Temporal.TimeZone.from(value);
            }).toThrowWithMessage(RangeError, "Invalid ISO date time");
        }
    });
});
