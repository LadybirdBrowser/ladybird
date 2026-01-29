describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainYearMonth.prototype.subtract).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        const result = plainYearMonth.subtract(new Temporal.Duration(51, 6));
        expect(result.equals(new Temporal.PlainYearMonth(1970, 1))).toBeTrue();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Temporal.PlainYearMonth.prototype.subtract.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });

    test("subtracting units other than years and months are not allowed", () => {
        const units = ["days", "hours", "minutes", "seconds", "milliseconds", "microseconds", "nanoseconds"];
        const plainYearMonth = new Temporal.PlainYearMonth(1970, 1);

        for (let unit of units) {
            expect(() => {
                plainYearMonth.subtract({ [unit]: 1 });
            }).toThrowWithMessage(RangeError, "Only years and months may be subtracted from Temporal.PlainYearMonth");
        }
    });
});
