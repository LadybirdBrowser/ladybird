describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Intl.DurationFormat.prototype.formatToParts).toHaveLength(1);
    });

    test("formats duration correctly", () => {
        const duration = {
            years: 1,
            months: 2,
            weeks: 3,
            days: 3,
            hours: 4,
            minutes: 5,
            seconds: 6,
            milliseconds: 7,
            microseconds: 8,
            nanoseconds: 9,
        };
        expect(new Intl.DurationFormat().formatToParts(duration)).toEqual([
            { type: "integer", value: "1", unit: "year" },
            { type: "literal", value: " ", unit: "year" },
            { type: "unit", value: "yr", unit: "year" },
            { type: "literal", value: ", " },
            { type: "integer", value: "2", unit: "month" },
            { type: "literal", value: " ", unit: "month" },
            { type: "unit", value: "mths", unit: "month" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "week" },
            { type: "literal", value: " ", unit: "week" },
            { type: "unit", value: "wks", unit: "week" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "day" },
            { type: "literal", value: " ", unit: "day" },
            { type: "unit", value: "days", unit: "day" },
            { type: "literal", value: ", " },
            { type: "integer", value: "4", unit: "hour" },
            { type: "literal", value: " ", unit: "hour" },
            { type: "unit", value: "hr", unit: "hour" },
            { type: "literal", value: ", " },
            { type: "integer", value: "5", unit: "minute" },
            { type: "literal", value: " ", unit: "minute" },
            { type: "unit", value: "min", unit: "minute" },
            { type: "literal", value: ", " },
            { type: "integer", value: "6", unit: "second" },
            { type: "literal", value: " ", unit: "second" },
            { type: "unit", value: "sec", unit: "second" },
            { type: "literal", value: ", " },
            { type: "integer", value: "7", unit: "millisecond" },
            { type: "literal", value: " ", unit: "millisecond" },
            { type: "unit", value: "ms", unit: "millisecond" },
            { type: "literal", value: ", " },
            { type: "integer", value: "8", unit: "microsecond" },
            { type: "literal", value: " ", unit: "microsecond" },
            { type: "unit", value: "μs", unit: "microsecond" },
            { type: "literal", value: ", " },
            { type: "integer", value: "9", unit: "nanosecond" },
            { type: "literal", value: " ", unit: "nanosecond" },
            { type: "unit", value: "ns", unit: "nanosecond" },
        ]);
        expect(new Intl.DurationFormat("en").formatToParts(duration)).toEqual([
            { type: "integer", value: "1", unit: "year" },
            { type: "literal", value: " ", unit: "year" },
            { type: "unit", value: "yr", unit: "year" },
            { type: "literal", value: ", " },
            { type: "integer", value: "2", unit: "month" },
            { type: "literal", value: " ", unit: "month" },
            { type: "unit", value: "mths", unit: "month" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "week" },
            { type: "literal", value: " ", unit: "week" },
            { type: "unit", value: "wks", unit: "week" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "day" },
            { type: "literal", value: " ", unit: "day" },
            { type: "unit", value: "days", unit: "day" },
            { type: "literal", value: ", " },
            { type: "integer", value: "4", unit: "hour" },
            { type: "literal", value: " ", unit: "hour" },
            { type: "unit", value: "hr", unit: "hour" },
            { type: "literal", value: ", " },
            { type: "integer", value: "5", unit: "minute" },
            { type: "literal", value: " ", unit: "minute" },
            { type: "unit", value: "min", unit: "minute" },
            { type: "literal", value: ", " },
            { type: "integer", value: "6", unit: "second" },
            { type: "literal", value: " ", unit: "second" },
            { type: "unit", value: "sec", unit: "second" },
            { type: "literal", value: ", " },
            { type: "integer", value: "7", unit: "millisecond" },
            { type: "literal", value: " ", unit: "millisecond" },
            { type: "unit", value: "ms", unit: "millisecond" },
            { type: "literal", value: ", " },
            { type: "integer", value: "8", unit: "microsecond" },
            { type: "literal", value: " ", unit: "microsecond" },
            { type: "unit", value: "μs", unit: "microsecond" },
            { type: "literal", value: ", " },
            { type: "integer", value: "9", unit: "nanosecond" },
            { type: "literal", value: " ", unit: "nanosecond" },
            { type: "unit", value: "ns", unit: "nanosecond" },
        ]);
        expect(new Intl.DurationFormat("en", { style: "long" }).formatToParts(duration)).toEqual([
            { type: "integer", value: "1", unit: "year" },
            { type: "literal", value: " ", unit: "year" },
            { type: "unit", value: "year", unit: "year" },
            { type: "literal", value: ", " },
            { type: "integer", value: "2", unit: "month" },
            { type: "literal", value: " ", unit: "month" },
            { type: "unit", value: "months", unit: "month" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "week" },
            { type: "literal", value: " ", unit: "week" },
            { type: "unit", value: "weeks", unit: "week" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "day" },
            { type: "literal", value: " ", unit: "day" },
            { type: "unit", value: "days", unit: "day" },
            { type: "literal", value: ", " },
            { type: "integer", value: "4", unit: "hour" },
            { type: "literal", value: " ", unit: "hour" },
            { type: "unit", value: "hours", unit: "hour" },
            { type: "literal", value: ", " },
            { type: "integer", value: "5", unit: "minute" },
            { type: "literal", value: " ", unit: "minute" },
            { type: "unit", value: "minutes", unit: "minute" },
            { type: "literal", value: ", " },
            { type: "integer", value: "6", unit: "second" },
            { type: "literal", value: " ", unit: "second" },
            { type: "unit", value: "seconds", unit: "second" },
            { type: "literal", value: ", " },
            { type: "integer", value: "7", unit: "millisecond" },
            { type: "literal", value: " ", unit: "millisecond" },
            { type: "unit", value: "milliseconds", unit: "millisecond" },
            { type: "literal", value: ", " },
            { type: "integer", value: "8", unit: "microsecond" },
            { type: "literal", value: " ", unit: "microsecond" },
            { type: "unit", value: "microseconds", unit: "microsecond" },
            { type: "literal", value: ", " },
            { type: "integer", value: "9", unit: "nanosecond" },
            { type: "literal", value: " ", unit: "nanosecond" },
            { type: "unit", value: "nanoseconds", unit: "nanosecond" },
        ]);
        expect(new Intl.DurationFormat("en", { style: "short" }).formatToParts(duration)).toEqual([
            { type: "integer", value: "1", unit: "year" },
            { type: "literal", value: " ", unit: "year" },
            { type: "unit", value: "yr", unit: "year" },
            { type: "literal", value: ", " },
            { type: "integer", value: "2", unit: "month" },
            { type: "literal", value: " ", unit: "month" },
            { type: "unit", value: "mths", unit: "month" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "week" },
            { type: "literal", value: " ", unit: "week" },
            { type: "unit", value: "wks", unit: "week" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "day" },
            { type: "literal", value: " ", unit: "day" },
            { type: "unit", value: "days", unit: "day" },
            { type: "literal", value: ", " },
            { type: "integer", value: "4", unit: "hour" },
            { type: "literal", value: " ", unit: "hour" },
            { type: "unit", value: "hr", unit: "hour" },
            { type: "literal", value: ", " },
            { type: "integer", value: "5", unit: "minute" },
            { type: "literal", value: " ", unit: "minute" },
            { type: "unit", value: "min", unit: "minute" },
            { type: "literal", value: ", " },
            { type: "integer", value: "6", unit: "second" },
            { type: "literal", value: " ", unit: "second" },
            { type: "unit", value: "sec", unit: "second" },
            { type: "literal", value: ", " },
            { type: "integer", value: "7", unit: "millisecond" },
            { type: "literal", value: " ", unit: "millisecond" },
            { type: "unit", value: "ms", unit: "millisecond" },
            { type: "literal", value: ", " },
            { type: "integer", value: "8", unit: "microsecond" },
            { type: "literal", value: " ", unit: "microsecond" },
            { type: "unit", value: "μs", unit: "microsecond" },
            { type: "literal", value: ", " },
            { type: "integer", value: "9", unit: "nanosecond" },
            { type: "literal", value: " ", unit: "nanosecond" },
            { type: "unit", value: "ns", unit: "nanosecond" },
        ]);
        expect(new Intl.DurationFormat("en", { style: "narrow" }).formatToParts(duration)).toEqual([
            { type: "integer", value: "1", unit: "year" },
            { type: "unit", value: "y", unit: "year" },
            { type: "literal", value: " " },
            { type: "integer", value: "2", unit: "month" },
            { type: "unit", value: "m", unit: "month" },
            { type: "literal", value: " " },
            { type: "integer", value: "3", unit: "week" },
            { type: "unit", value: "w", unit: "week" },
            { type: "literal", value: " " },
            { type: "integer", value: "3", unit: "day" },
            { type: "unit", value: "d", unit: "day" },
            { type: "literal", value: " " },
            { type: "integer", value: "4", unit: "hour" },
            { type: "unit", value: "h", unit: "hour" },
            { type: "literal", value: " " },
            { type: "integer", value: "5", unit: "minute" },
            { type: "unit", value: "m", unit: "minute" },
            { type: "literal", value: " " },
            { type: "integer", value: "6", unit: "second" },
            { type: "unit", value: "s", unit: "second" },
            { type: "literal", value: " " },
            { type: "integer", value: "7", unit: "millisecond" },
            { type: "unit", value: "ms", unit: "millisecond" },
            { type: "literal", value: " " },
            { type: "integer", value: "8", unit: "microsecond" },
            { type: "unit", value: "μs", unit: "microsecond" },
            { type: "literal", value: " " },
            { type: "integer", value: "9", unit: "nanosecond" },
            { type: "unit", value: "ns", unit: "nanosecond" },
        ]);
        expect(new Intl.DurationFormat("en", { style: "digital" }).formatToParts(duration)).toEqual(
            [
                { type: "integer", value: "1", unit: "year" },
                { type: "literal", value: " ", unit: "year" },
                { type: "unit", value: "yr", unit: "year" },
                { type: "literal", value: ", " },
                { type: "integer", value: "2", unit: "month" },
                { type: "literal", value: " ", unit: "month" },
                { type: "unit", value: "mths", unit: "month" },
                { type: "literal", value: ", " },
                { type: "integer", value: "3", unit: "week" },
                { type: "literal", value: " ", unit: "week" },
                { type: "unit", value: "wks", unit: "week" },
                { type: "literal", value: ", " },
                { type: "integer", value: "3", unit: "day" },
                { type: "literal", value: " ", unit: "day" },
                { type: "unit", value: "days", unit: "day" },
                { type: "literal", value: ", " },
                { type: "integer", value: "4", unit: "hour" },
                { type: "literal", value: ":" },
                { type: "integer", value: "05", unit: "minute" },
                { type: "literal", value: ":" },
                { type: "integer", value: "06", unit: "second" },
                { type: "decimal", value: ".", unit: "second" },
                { type: "fraction", value: "007008009", unit: "second" },
            ]
        );
        expect(
            new Intl.DurationFormat("en", {
                style: "narrow",
                nanoseconds: "numeric",
                fractionalDigits: 3,
            }).formatToParts(duration)
        ).toEqual([
            { type: "integer", value: "1", unit: "year" },
            { type: "unit", value: "y", unit: "year" },
            { type: "literal", value: " " },
            { type: "integer", value: "2", unit: "month" },
            { type: "unit", value: "m", unit: "month" },
            { type: "literal", value: " " },
            { type: "integer", value: "3", unit: "week" },
            { type: "unit", value: "w", unit: "week" },
            { type: "literal", value: " " },
            { type: "integer", value: "3", unit: "day" },
            { type: "unit", value: "d", unit: "day" },
            { type: "literal", value: " " },
            { type: "integer", value: "4", unit: "hour" },
            { type: "unit", value: "h", unit: "hour" },
            { type: "literal", value: " " },
            { type: "integer", value: "5", unit: "minute" },
            { type: "unit", value: "m", unit: "minute" },
            { type: "literal", value: " " },
            { type: "integer", value: "6", unit: "second" },
            { type: "unit", value: "s", unit: "second" },
            { type: "literal", value: " " },
            { type: "integer", value: "7", unit: "millisecond" },
            { type: "unit", value: "ms", unit: "millisecond" },
            { type: "literal", value: " " },
            { type: "integer", value: "8", unit: "microsecond" },
            { type: "decimal", value: ".", unit: "microsecond" },
            { type: "fraction", value: "009", unit: "microsecond" },
            { type: "unit", value: "μs", unit: "microsecond" },
        ]);

        expect(new Intl.DurationFormat("de", { style: "long" }).formatToParts(duration)).toEqual([
            { type: "integer", value: "1", unit: "year" },
            { type: "literal", value: " ", unit: "year" },
            { type: "unit", value: "Jahr", unit: "year" },
            { type: "literal", value: ", " },
            { type: "integer", value: "2", unit: "month" },
            { type: "literal", value: " ", unit: "month" },
            { type: "unit", value: "Monate", unit: "month" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "week" },
            { type: "literal", value: " ", unit: "week" },
            { type: "unit", value: "Wochen", unit: "week" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "day" },
            { type: "literal", value: " ", unit: "day" },
            { type: "unit", value: "Tage", unit: "day" },
            { type: "literal", value: ", " },
            { type: "integer", value: "4", unit: "hour" },
            { type: "literal", value: " ", unit: "hour" },
            { type: "unit", value: "Stunden", unit: "hour" },
            { type: "literal", value: ", " },
            { type: "integer", value: "5", unit: "minute" },
            { type: "literal", value: " ", unit: "minute" },
            { type: "unit", value: "Minuten", unit: "minute" },
            { type: "literal", value: ", " },
            { type: "integer", value: "6", unit: "second" },
            { type: "literal", value: " ", unit: "second" },
            { type: "unit", value: "Sekunden", unit: "second" },
            { type: "literal", value: ", " },
            { type: "integer", value: "7", unit: "millisecond" },
            { type: "literal", value: " ", unit: "millisecond" },
            { type: "unit", value: "Millisekunden", unit: "millisecond" },
            { type: "literal", value: ", " },
            { type: "integer", value: "8", unit: "microsecond" },
            { type: "literal", value: " ", unit: "microsecond" },
            { type: "unit", value: "Mikrosekunden", unit: "microsecond" },
            { type: "literal", value: " und " },
            { type: "integer", value: "9", unit: "nanosecond" },
            { type: "literal", value: " ", unit: "nanosecond" },
            { type: "unit", value: "Nanosekunden", unit: "nanosecond" },
        ]);
        expect(new Intl.DurationFormat("de", { style: "short" }).formatToParts(duration)).toEqual([
            { type: "integer", value: "1", unit: "year" },
            { type: "literal", value: " ", unit: "year" },
            { type: "unit", value: "J", unit: "year" },
            { type: "literal", value: ", " },
            { type: "integer", value: "2", unit: "month" },
            { type: "literal", value: " ", unit: "month" },
            { type: "unit", value: "Mon.", unit: "month" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "week" },
            { type: "literal", value: " ", unit: "week" },
            { type: "unit", value: "Wo.", unit: "week" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "day" },
            { type: "literal", value: " ", unit: "day" },
            { type: "unit", value: "Tg.", unit: "day" },
            { type: "literal", value: ", " },
            { type: "integer", value: "4", unit: "hour" },
            { type: "literal", value: " ", unit: "hour" },
            { type: "unit", value: "Std.", unit: "hour" },
            { type: "literal", value: ", " },
            { type: "integer", value: "5", unit: "minute" },
            { type: "literal", value: " ", unit: "minute" },
            { type: "unit", value: "Min.", unit: "minute" },
            { type: "literal", value: ", " },
            { type: "integer", value: "6", unit: "second" },
            { type: "literal", value: " ", unit: "second" },
            { type: "unit", value: "Sek.", unit: "second" },
            { type: "literal", value: ", " },
            { type: "integer", value: "7", unit: "millisecond" },
            { type: "literal", value: " ", unit: "millisecond" },
            { type: "unit", value: "ms", unit: "millisecond" },
            { type: "literal", value: ", " },
            { type: "integer", value: "8", unit: "microsecond" },
            { type: "literal", value: " ", unit: "microsecond" },
            { type: "unit", value: "μs", unit: "microsecond" },
            { type: "literal", value: " und " },
            { type: "integer", value: "9", unit: "nanosecond" },
            { type: "literal", value: " ", unit: "nanosecond" },
            { type: "unit", value: "ns", unit: "nanosecond" },
        ]);
        expect(new Intl.DurationFormat("de", { style: "narrow" }).formatToParts(duration)).toEqual([
            { type: "integer", value: "1", unit: "year" },
            { type: "literal", value: " ", unit: "year" },
            { type: "unit", value: "J", unit: "year" },
            { type: "literal", value: ", " },
            { type: "integer", value: "2", unit: "month" },
            { type: "literal", value: " ", unit: "month" },
            { type: "unit", value: "M", unit: "month" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "week" },
            { type: "literal", value: " ", unit: "week" },
            { type: "unit", value: "W", unit: "week" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "day" },
            { type: "literal", value: " ", unit: "day" },
            { type: "unit", value: "T", unit: "day" },
            { type: "literal", value: ", " },
            { type: "integer", value: "4", unit: "hour" },
            { type: "literal", value: " ", unit: "hour" },
            { type: "unit", value: "Std.", unit: "hour" },
            { type: "literal", value: ", " },
            { type: "integer", value: "5", unit: "minute" },
            { type: "literal", value: " ", unit: "minute" },
            { type: "unit", value: "Min.", unit: "minute" },
            { type: "literal", value: ", " },
            { type: "integer", value: "6", unit: "second" },
            { type: "literal", value: " ", unit: "second" },
            { type: "unit", value: "Sek.", unit: "second" },
            { type: "literal", value: ", " },
            { type: "integer", value: "7", unit: "millisecond" },
            { type: "literal", value: " ", unit: "millisecond" },
            { type: "unit", value: "ms", unit: "millisecond" },
            { type: "literal", value: ", " },
            { type: "integer", value: "8", unit: "microsecond" },
            { type: "literal", value: " ", unit: "microsecond" },
            { type: "unit", value: "μs", unit: "microsecond" },
            { type: "literal", value: " und " },
            { type: "integer", value: "9", unit: "nanosecond" },
            { type: "literal", value: " ", unit: "nanosecond" },
            { type: "unit", value: "ns", unit: "nanosecond" },
        ]);
        expect(new Intl.DurationFormat("de", { style: "digital" }).formatToParts(duration)).toEqual(
            [
                { type: "integer", value: "1", unit: "year" },
                { type: "literal", value: " ", unit: "year" },
                { type: "unit", value: "J", unit: "year" },
                { type: "literal", value: ", " },
                { type: "integer", value: "2", unit: "month" },
                { type: "literal", value: " ", unit: "month" },
                { type: "unit", value: "Mon.", unit: "month" },
                { type: "literal", value: ", " },
                { type: "integer", value: "3", unit: "week" },
                { type: "literal", value: " ", unit: "week" },
                { type: "unit", value: "Wo.", unit: "week" },
                { type: "literal", value: ", " },
                { type: "integer", value: "3", unit: "day" },
                { type: "literal", value: " ", unit: "day" },
                { type: "unit", value: "Tg.", unit: "day" },
                { type: "literal", value: " und " },
                { type: "integer", value: "4", unit: "hour" },
                { type: "literal", value: ":" },
                { type: "integer", value: "05", unit: "minute" },
                { type: "literal", value: ":" },
                { type: "integer", value: "06", unit: "second" },
                { type: "decimal", value: ",", unit: "second" },
                { type: "fraction", value: "007008009", unit: "second" },
            ]
        );
        expect(
            new Intl.DurationFormat("de", {
                style: "narrow",
                nanoseconds: "numeric",
                fractionalDigits: 3,
            }).formatToParts(duration)
        ).toEqual([
            { type: "integer", value: "1", unit: "year" },
            { type: "literal", value: " ", unit: "year" },
            { type: "unit", value: "J", unit: "year" },
            { type: "literal", value: ", " },
            { type: "integer", value: "2", unit: "month" },
            { type: "literal", value: " ", unit: "month" },
            { type: "unit", value: "M", unit: "month" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "week" },
            { type: "literal", value: " ", unit: "week" },
            { type: "unit", value: "W", unit: "week" },
            { type: "literal", value: ", " },
            { type: "integer", value: "3", unit: "day" },
            { type: "literal", value: " ", unit: "day" },
            { type: "unit", value: "T", unit: "day" },
            { type: "literal", value: ", " },
            { type: "integer", value: "4", unit: "hour" },
            { type: "literal", value: " ", unit: "hour" },
            { type: "unit", value: "Std.", unit: "hour" },
            { type: "literal", value: ", " },
            { type: "integer", value: "5", unit: "minute" },
            { type: "literal", value: " ", unit: "minute" },
            { type: "unit", value: "Min.", unit: "minute" },
            { type: "literal", value: ", " },
            { type: "integer", value: "6", unit: "second" },
            { type: "literal", value: " ", unit: "second" },
            { type: "unit", value: "Sek.", unit: "second" },
            { type: "literal", value: ", " },
            { type: "integer", value: "7", unit: "millisecond" },
            { type: "literal", value: " ", unit: "millisecond" },
            { type: "unit", value: "ms", unit: "millisecond" },
            { type: "literal", value: " und " },
            { type: "integer", value: "8", unit: "microsecond" },
            { type: "decimal", value: ",", unit: "microsecond" },
            { type: "fraction", value: "009", unit: "microsecond" },
            { type: "literal", value: " ", unit: "microsecond" },
            { type: "unit", value: "μs", unit: "microsecond" },
        ]);
    });
});

describe("errors", () => {
    test("non-object duration records", () => {
        expect(() => {
            new Intl.DurationFormat().formatToParts("hello");
        }).toThrowWithMessage(RangeError, "is not an object");

        [-100, Infinity, NaN, 152n, Symbol("foo"), true, null, undefined].forEach(value => {
            expect(() => {
                new Intl.DurationFormat().formatToParts(value);
            }).toThrowWithMessage(TypeError, "is not an object");
        });
    });

    test("empty duration record", () => {
        expect(() => {
            new Intl.DurationFormat().formatToParts({});
        }).toThrowWithMessage(TypeError, "Invalid duration-like object");

        expect(() => {
            new Intl.DurationFormat().formatToParts({ foo: 123 });
        }).toThrowWithMessage(TypeError, "Invalid duration-like object");
    });

    test("non-integral duration fields", () => {
        [
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
        ].forEach(field => {
            expect(() => {
                new Intl.DurationFormat().formatToParts({ [field]: 1.5 });
            }).toThrowWithMessage(
                RangeError,
                `Invalid value for duration property '${field}': must be an integer, got 1.5`
            );

            expect(() => {
                new Intl.DurationFormat().formatToParts({ [field]: -Infinity });
            }).toThrowWithMessage(
                RangeError,
                `Invalid value for duration property '${field}': must be an integer, got -Infinity`
            );
        });
    });

    test("inconsistent field signs", () => {
        expect(() => {
            new Intl.DurationFormat().formatToParts({ years: 1, months: -1 });
        }).toThrowWithMessage(RangeError, "Invalid duration-like object");
    });
});
