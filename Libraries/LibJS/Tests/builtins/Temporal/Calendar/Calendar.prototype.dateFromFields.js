describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Calendar.prototype.dateFromFields).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const date = calendar.dateFromFields({ year: 2000, month: 5, day: 2 });
        expect(date.calendar).toBe(calendar);
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
        calendar.dateFromFields(fields, options);
        expect(operations).toHaveLength(5);
        expect(operations[0]).toBe("get day");
        expect(operations[1]).toBe("get month");
        expect(operations[2]).toBe("get monthCode");
        expect(operations[3]).toBe("get year");
        expect(operations[4]).toBe("get overflow");
    });
});
