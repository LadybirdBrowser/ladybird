describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainTime.prototype.toLocaleString).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainTime = new Temporal.PlainTime(18, 14, 47, 123, 456, 789);
        expect(plainTime.toLocaleString()).toBe("6:14:47 PM");
    });

    test("ignores time zones", () => {
        const plainTime = new Temporal.PlainTime(18, 14, 47, 123, 456, 789);

        const result1 = plainTime.toLocaleString("en-US");
        const result2 = plainTime.toLocaleString("en-US", { timeZone: "UTC" });
        const result3 = plainTime.toLocaleString("en-US", { timeZone: "Pacific/Apia" });

        expect(result1).toBe(result2);
        expect(result1).toBe(result3);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainTime object", () => {
        expect(() => {
            Temporal.PlainTime.prototype.toLocaleString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainTime");
    });
});
