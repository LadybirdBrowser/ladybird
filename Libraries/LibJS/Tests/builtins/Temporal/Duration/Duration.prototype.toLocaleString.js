describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.Duration.prototype.toLocaleString).toHaveLength(0);
    });

    test("basic functionality", () => {
        expect(new Temporal.Duration(1, 2, 3, 4, 5, 6, 7, 8, 9, 10).toLocaleString()).toBe(
            "P1Y2M3W4DT5H6M7.00800901S"
        );
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Duration object", () => {
        expect(() => {
            Temporal.Duration.prototype.toLocaleString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Duration");
    });
});
