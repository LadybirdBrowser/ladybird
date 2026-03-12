describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDate.prototype.toLocaleString).toHaveLength(0);
    });

    test("basic functionality", () => {
        let plainDate;

        plainDate = new Temporal.PlainDate(2021, 7, 6, "gregory");
        expect(plainDate.toLocaleString()).toBe("7/6/2021");
    });

    test("ignores time zones", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 6, "gregory");

        const result1 = plainDate.toLocaleString("en-US");
        const result2 = plainDate.toLocaleString("en-US", { timeZone: "UTC" });
        const result3 = plainDate.toLocaleString("en-US", { timeZone: "Pacific/Apia" });

        expect(result1).toBe(result2);
        expect(result1).toBe(result3);
    });

    test("DateTimeFormat and Temporal agree on lunisolar calendar dates", () => {
        function verifyDateTimeFormatAgreesWithTemporal(calendar) {
            const dtf = new Intl.DateTimeFormat("en", {
                calendar,
                timeZone: "UTC",
                year: "numeric",
                month: "numeric",
                day: "numeric",
            });

            // Test a range of years that covers leap month variations.
            for (let isoYear = 2020; isoYear <= 2030; ++isoYear) {
                const { year, monthsInYear } = new Temporal.PlainDate(isoYear, 1, 1, calendar);

                for (let month = 1; month <= monthsInYear; ++month) {
                    const date = Temporal.PlainDate.from({
                        calendar,
                        year,
                        month,
                        day: 1,
                    });

                    const { epochMilliseconds } = date.withCalendar("iso8601").toZonedDateTime("UTC");
                    const parts = dtf.formatToParts(epochMilliseconds);

                    const yearPart = parts.find(({ type }) => type === "year" || type === "relatedYear");
                    const monthPart = parts.find(({ type }) => type === "month");
                    const dayPart = parts.find(({ type }) => type === "day");

                    const formattedYear = +yearPart.value;
                    const formattedDay = +dayPart.value;
                    const formattedMonth = +monthPart.value;

                    let formattedMonthCode;
                    if (Number.isInteger(formattedMonth)) {
                        formattedMonthCode = `M${String(formattedMonth).padStart(2, "0")}`;
                    } else {
                        const monthNumber = Number.parseInt(monthPart.value);
                        formattedMonthCode = `M${String(monthNumber).padStart(2, "0")}L`;
                    }

                    expect(formattedYear).toBe(date.year);
                    expect(formattedMonthCode).toBe(date.monthCode);
                    expect(formattedDay).toBe(date.day);
                }
            }
        }

        verifyDateTimeFormatAgreesWithTemporal("chinese");
        verifyDateTimeFormatAgreesWithTemporal("dangi");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Temporal.PlainDate.prototype.toLocaleString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
