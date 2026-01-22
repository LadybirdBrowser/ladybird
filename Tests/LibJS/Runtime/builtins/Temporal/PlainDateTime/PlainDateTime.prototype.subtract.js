describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDateTime.prototype.subtract).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 6, 18, 14, 47);
        const result = plainDateTime.subtract(new Temporal.Duration(51, 6, 0, 5, 18, 14, 47));
        expect(result.equals(new Temporal.PlainDateTime(1970, 1, 1)));
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDateTime object", () => {
        expect(() => {
            Temporal.PlainDateTime.prototype.subtract.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDateTime");
    });
});
