describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDate.prototype.add).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(1970, 1, 1);
        const result = plainDate.add(new Temporal.Duration(51, 6, 0, 5));
        expect(result.equals(new Temporal.PlainDate(2021, 7, 6))).toBeTrue();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Temporal.PlainDate.prototype.add.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
