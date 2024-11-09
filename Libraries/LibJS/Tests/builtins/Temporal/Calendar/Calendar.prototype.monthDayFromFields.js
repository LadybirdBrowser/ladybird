describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Calendar.prototype.monthDayFromFields).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const plainMonthDay = calendar.monthDayFromFields({ year: 2021, month: 7, day: 6 });
        expect(plainMonthDay.calendar).toBe(calendar);
        expect(plainMonthDay.monthCode).toBe("M07");
        expect(plainMonthDay.day).toBe(6);

        const fields = plainMonthDay.getISOFields();
        expect(fields.isoYear).toBe(1972); // No, this isn't a mistake
    });

    test("with monthCode", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const plainMonthDay = calendar.monthDayFromFields({ monthCode: "M07", day: 6 });
        expect(plainMonthDay.calendar).toBe(calendar);
        expect(plainMonthDay.monthCode).toBe("M07");
        expect(plainMonthDay.day).toBe(6);

        const fields = plainMonthDay.getISOFields();
        expect(fields.isoYear).toBe(1972);
    });

    test("gets overflow after temporal fields", () => {
        const operations = [];
        const calendar = new Temporal.Calendar("iso8601");

        const fields = {
            get day() {
                operations.push("get day");
                return 3;
            },

            get month() {
                operations.push("get month");
                return 10;
            },

            get monthCode() {
                operations.push("get monthCode");
                return "M10";
            },

            get year() {
                operations.push("get year");
                return 2022;
            },
        };

        const options = {
            get overflow() {
                operations.push("get overflow");
                return "constrain";
            },
        };

        expect(operations).toHaveLength(0);
        calendar.monthDayFromFields(fields, options);
        expect(operations).toHaveLength(5);
        expect(operations[0]).toBe("get day");
        expect(operations[1]).toBe("get month");
        expect(operations[2]).toBe("get monthCode");
        expect(operations[3]).toBe("get year");
        expect(operations[4]).toBe("get overflow");
    });
});

describe("errors", () => {
    test("first argument must be an object", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(() => {
            calendar.monthDayFromFields(42);
        }).toThrowWithMessage(TypeError, "42 is not an object");
    });

    test("month or monthCode field is required", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(() => {
            calendar.monthDayFromFields({ year: 2021, day: 1 });
        }).toThrowWithMessage(TypeError, "Required property month is missing or undefined");
    });

    test("day field is required", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(() => {
            calendar.monthDayFromFields({ year: 2021, month: 7 });
        }).toThrowWithMessage(TypeError, "Required property day is missing or undefined");
    });

    test("monthCode or year field is required when month is given", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(() => {
            calendar.monthDayFromFields({ month: 7, day: 6 });
        }).toThrowWithMessage(
            TypeError,
            "Required property monthCode or year is missing or undefined"
        );
    });
});
