describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.Duration.prototype.toLocaleString).toHaveLength(0);
    });

    test("basic functionality", () => {
        expect(new Temporal.Duration(1, 2, 3, 4, 5, 6, 7, 8, 9, 10).toLocaleString()).toBe(
            "1 yr, 2 mths, 3 wks, 4 days, 5 hr, 6 min, 7 sec, 8 ms, 9 μs, 10 ns"
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
