describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDateTime.prototype.toLocaleString).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 1, 33, 5, 100, 200, 300, "gregory");
        expect(plainDateTime.toLocaleString()).toBe("11/3/2021, 1:33:05 AM");
    });

    test("ignores time zones", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 1, 33, 5, 100, 200, 300, "gregory");

        const result1 = plainDateTime.toLocaleString("en-US");
        const result2 = plainDateTime.toLocaleString("en-US", { timeZone: "UTC" });
        const result3 = plainDateTime.toLocaleString("en-US", { timeZone: "Pacific/Apia" });

        expect(result1).toBe(result2);
        expect(result1).toBe(result3);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDateTime object", () => {
        expect(() => {
            Temporal.PlainDateTime.prototype.toLocaleString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDateTime");
    });
});
