describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.TimeZone.prototype.toString).toHaveLength(0);
    });

    test("basic functionality", () => {
        const values = [
            ["utc", "UTC"],
            ["Utc", "UTC"],
            ["UTC", "UTC"],
            ["GMT", "UTC"],
            ["Etc/UTC", "UTC"],
            ["Etc/GMT", "UTC"],
            ["Europe/London", "Europe/London"],
            ["Australia/Canberra", "Australia/Sydney"],
            ["+00:00", "+00:00"],
            ["+00:00:00", "+00:00"],
            ["+00:00:00.000", "+00:00"],
            ["+12:34:56.789", "+12:34:56.789"],
            ["+12:34:56.789000", "+12:34:56.789"],
            ["-01:00", "-01:00"],
        ];
        for (const [arg, expected] of values) {
            expect(new Temporal.TimeZone(arg).toString()).toBe(expected);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.TimeZone object", () => {
        expect(() => {
            Temporal.TimeZone.prototype.toString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.TimeZone");
    });
});
