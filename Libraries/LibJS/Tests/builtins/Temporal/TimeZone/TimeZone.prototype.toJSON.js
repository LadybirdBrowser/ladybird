describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.TimeZone.prototype.toJSON).toHaveLength(0);
    });

    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        expect(timeZone.toJSON()).toBe("UTC");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.TimeZone object", () => {
        expect(() => {
            Temporal.TimeZone.prototype.toJSON.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.TimeZone");
    });
});
