describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDateTime.prototype.toLocaleString).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 1, 33, 5, 100, 200, 300);
        expect(plainDateTime.toLocaleString()).toBe("2021-11-03T01:33:05.1002003");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDateTime object", () => {
        expect(() => {
            Temporal.PlainDateTime.prototype.toLocaleString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDateTime");
    });
});
