describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.Calendar.prototype.toString).toHaveLength(0);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(calendar.toString()).toBe("iso8601");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Calendar object", () => {
        expect(() => {
            Temporal.Calendar.prototype.toString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Calendar");
    });
});
