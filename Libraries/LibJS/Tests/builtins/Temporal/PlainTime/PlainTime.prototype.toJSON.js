describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainTime.prototype.toJSON).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainTime = new Temporal.PlainTime(18, 14, 47, 123, 456, 789);
        expect(plainTime.toJSON()).toBe("18:14:47.123456789");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainTime object", () => {
        expect(() => {
            Temporal.PlainTime.prototype.toJSON.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainTime");
    });
});
