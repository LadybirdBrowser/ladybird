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
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Temporal.PlainDate.prototype.toLocaleString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
