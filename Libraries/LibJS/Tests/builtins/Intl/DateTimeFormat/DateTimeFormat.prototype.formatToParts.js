describe("errors", () => {
    test("called on non-DateTimeFormat object", () => {
        expect(() => {
            Intl.DateTimeFormat.prototype.formatToParts(1);
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.DateTimeFormat");
    });

    test("called with value that cannot be converted to a number", () => {
        expect(() => {
            Intl.DateTimeFormat().formatToParts(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            Intl.DateTimeFormat().formatToParts(1n);
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");
    });

    test("time value cannot be clipped", () => {
        expect(() => {
            Intl.DateTimeFormat().formatToParts(NaN);
        }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");

        expect(() => {
            Intl.DateTimeFormat().formatToParts(-8.65e15);
        }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");

        expect(() => {
            Intl.DateTimeFormat().formatToParts(8.65e15);
        }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");
    });
});

const d = Date.UTC(1989, 0, 23, 7, 8, 9, 45);

describe("dateStyle", () => {
    test("full", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "full", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "weekday", value: "Monday" },
            { type: "literal", value: ", " },
            { type: "month", value: "January" },
            { type: "literal", value: " " },
            { type: "day", value: "23" },
            { type: "literal", value: ", " },
            { type: "year", value: "1989" },
        ]);

        const ar = new Intl.DateTimeFormat("ar", { dateStyle: "full", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "weekday", value: "الاثنين" },
            { type: "literal", value: "، " },
            { type: "day", value: "٢٣" },
            { type: "literal", value: " " },
            { type: "month", value: "يناير" },
            { type: "literal", value: " " },
            { type: "year", value: "١٩٨٩" },
        ]);
    });

    test("long", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "long", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "month", value: "January" },
            { type: "literal", value: " " },
            { type: "day", value: "23" },
            { type: "literal", value: ", " },
            { type: "year", value: "1989" },
        ]);

        const ar = new Intl.DateTimeFormat("ar", { dateStyle: "long", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "day", value: "٢٣" },
            { type: "literal", value: " " },
            { type: "month", value: "يناير" },
            { type: "literal", value: " " },
            { type: "year", value: "١٩٨٩" },
        ]);
    });

    test("medium", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "medium", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "month", value: "Jan" },
            { type: "literal", value: " " },
            { type: "day", value: "23" },
            { type: "literal", value: ", " },
            { type: "year", value: "1989" },
        ]);

        const ar = new Intl.DateTimeFormat("ar", { dateStyle: "medium", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "day", value: "٢٣" },
            { type: "literal", value: "‏/" },
            { type: "month", value: "٠١" },
            { type: "literal", value: "‏/" },
            { type: "year", value: "١٩٨٩" },
        ]);
    });

    test("short", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "short", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "month", value: "1" },
            { type: "literal", value: "/" },
            { type: "day", value: "23" },
            { type: "literal", value: "/" },
            { type: "year", value: "89" },
        ]);

        const ar = new Intl.DateTimeFormat("ar", { dateStyle: "short", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "day", value: "٢٣" },
            { type: "literal", value: "‏/" },
            { type: "month", value: "١" },
            { type: "literal", value: "‏/" },
            { type: "year", value: "١٩٨٩" },
        ]);
    });
});

describe("timeStyle", () => {
    test("full", () => {
        const en = new Intl.DateTimeFormat("en", { timeStyle: "full", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "hour", value: "7" },
            { type: "literal", value: ":" },
            { type: "minute", value: "08" },
            { type: "literal", value: ":" },
            { type: "second", value: "09" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "AM" },
            { type: "literal", value: " " },
            { type: "timeZoneName", value: "Coordinated Universal Time" },
        ]);

        const ar = new Intl.DateTimeFormat("ar", { timeStyle: "full", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "hour", value: "٧" },
            { type: "literal", value: ":" },
            { type: "minute", value: "٠٨" },
            { type: "literal", value: ":" },
            { type: "second", value: "٠٩" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "ص" },
            { type: "literal", value: " " },
            { type: "timeZoneName", value: "التوقيت العالمي المنسق" },
        ]);
    });

    test("long", () => {
        const en = new Intl.DateTimeFormat("en", { timeStyle: "long", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "hour", value: "7" },
            { type: "literal", value: ":" },
            { type: "minute", value: "08" },
            { type: "literal", value: ":" },
            { type: "second", value: "09" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "AM" },
            { type: "literal", value: " " },
            { type: "timeZoneName", value: "UTC" },
        ]);

        const ar = new Intl.DateTimeFormat("ar", { timeStyle: "long", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "hour", value: "٧" },
            { type: "literal", value: ":" },
            { type: "minute", value: "٠٨" },
            { type: "literal", value: ":" },
            { type: "second", value: "٠٩" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "ص" },
            { type: "literal", value: " " },
            { type: "timeZoneName", value: "UTC" },
        ]);
    });

    test("medium", () => {
        const en = new Intl.DateTimeFormat("en", { timeStyle: "medium", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "hour", value: "7" },
            { type: "literal", value: ":" },
            { type: "minute", value: "08" },
            { type: "literal", value: ":" },
            { type: "second", value: "09" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "AM" },
        ]);

        const ar = new Intl.DateTimeFormat("ar", { timeStyle: "medium", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "hour", value: "٧" },
            { type: "literal", value: ":" },
            { type: "minute", value: "٠٨" },
            { type: "literal", value: ":" },
            { type: "second", value: "٠٩" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "ص" },
        ]);
    });

    test("short", () => {
        const en = new Intl.DateTimeFormat("en", { timeStyle: "short", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "hour", value: "7" },
            { type: "literal", value: ":" },
            { type: "minute", value: "08" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "AM" },
        ]);

        const ar = new Intl.DateTimeFormat("ar", { timeStyle: "short", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "hour", value: "٧" },
            { type: "literal", value: ":" },
            { type: "minute", value: "٠٨" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "ص" },
        ]);
    });
});

describe("special cases", () => {
    test("dayPeriod", () => {
        const en = new Intl.DateTimeFormat("en", {
            dayPeriod: "long",
            hour: "numeric",
            timeZone: "UTC",
        });
        expect(en.formatToParts(d)).toEqual([
            { type: "hour", value: "7" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "in the morning" },
        ]);

        const ar = new Intl.DateTimeFormat("ar", {
            dayPeriod: "long",
            hour: "numeric",
            timeZone: "UTC",
        });
        expect(ar.formatToParts(d)).toEqual([
            { type: "hour", value: "٧" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "صباحًا" },
        ]);
    });

    test("fractionalSecondDigits", () => {
        const en = new Intl.DateTimeFormat("en", {
            fractionalSecondDigits: 3,
            second: "numeric",
            minute: "numeric",
            timeZone: "UTC",
        });
        expect(en.formatToParts(d)).toEqual([
            { type: "minute", value: "08" },
            { type: "literal", value: ":" },
            { type: "second", value: "09" },
            { type: "literal", value: "." },
            { type: "fractionalSecond", value: "045" },
        ]);

        const ar = new Intl.DateTimeFormat("ar", {
            fractionalSecondDigits: 3,
            second: "numeric",
            minute: "numeric",
            timeZone: "UTC",
        });
        expect(ar.formatToParts(d)).toEqual([
            { type: "minute", value: "٠٨" },
            { type: "literal", value: ":" },
            { type: "second", value: "٠٩" },
            { type: "literal", value: "٫" },
            { type: "fractionalSecond", value: "٠٤٥" },
        ]);
    });
});
