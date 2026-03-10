describe("correct behavior", () => {
    test("basic functionality", () => {
        const date = new Temporal.PlainDate(2021, 7, 23);
        expect(date.monthCode).toBe("M07");
    });

    test("gregory calendar month codes", () => {
        let plainDate = new Temporal.PlainDate(2024, 1, 1, "gregory");
        expect(plainDate.monthCode).toBe("M01");

        plainDate = new Temporal.PlainDate(2024, 12, 1, "gregory");
        expect(plainDate.monthCode).toBe("M12");
    });

    test("hebrew calendar leap month code", () => {
        // 2024-02-11 falls in Adar I (M05L) of Hebrew year 5784 (a leap year).
        const plainDate = Temporal.PlainDate.from("2024-02-11[u-ca=hebrew]");
        expect(plainDate.monthCode).toBe("M05L");
    });

    test("chinese calendar regular month codes", () => {
        // 2024-02-10 is Chinese New Year (M01 day 1).
        const plainDate = Temporal.PlainDate.from("2024-02-10[u-ca=chinese]");
        expect(plainDate.monthCode).toBe("M01");
    });

    test("chinese calendar leap month code", () => {
        // 2023 has a Chinese leap month after M02.
        // 2023-03-22 falls in leap M02.
        const plainDate = Temporal.PlainDate.from("2023-03-22[u-ca=chinese]");
        expect(plainDate.monthCode).toBe("M02L");
    });

    test("chinese calendar leap month codes across decades", () => {
        const leapMonthCases = [
            { year: 2001, month: 5, monthCode: "M04L" },
            { year: 2004, month: 3, monthCode: "M02L" },
            { year: 2006, month: 8, monthCode: "M07L" },
            { year: 2009, month: 6, monthCode: "M05L" },
            { year: 2012, month: 5, monthCode: "M04L" },
            { year: 2017, month: 7, monthCode: "M06L" },
            { year: 2020, month: 5, monthCode: "M04L" },
            { year: 2023, month: 3, monthCode: "M02L" },
            { year: 2025, month: 7, monthCode: "M06L" },
            { year: 2028, month: 6, monthCode: "M05L" },
        ];

        for (const { year, month, monthCode } of leapMonthCases) {
            const fromMonth = Temporal.PlainDate.from({
                year,
                month,
                day: 1,
                calendar: "chinese",
            });
            expect(fromMonth.monthCode).toBe(monthCode);
            expect(fromMonth.month).toBe(month);

            const fromCode = Temporal.PlainDate.from({
                year,
                monthCode,
                day: 1,
                calendar: "chinese",
            });
            expect(fromCode.monthCode).toBe(monthCode);
            expect(fromCode.month).toBe(month);

            expect(fromMonth.equals(fromCode)).toBeTrue();
        }
    });

    test("coptic calendar 13th month", () => {
        // The Coptic calendar has a 13th month (Nasie).
        // 2024-09-06 falls in month 13 of Coptic year 1740.
        const plainDate = Temporal.PlainDate.from("2024-09-06[u-ca=coptic]");
        expect(plainDate.monthCode).toBe("M13");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "monthCode", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
