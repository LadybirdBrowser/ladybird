describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Temporal.Duration();
        }).toThrowWithMessage(TypeError, "Temporal.Duration constructor must be called with 'new'");
    });

    test("cannot mix arguments with different signs", () => {
        expect(() => {
            new Temporal.Duration(-1, 1);
        }).toThrowWithMessage(RangeError, "Invalid duration");
        expect(() => {
            new Temporal.Duration(1, -1);
        }).toThrowWithMessage(RangeError, "Invalid duration");
    });

    test("cannot pass Infinity", () => {
        expect(() => {
            new Temporal.Duration(Infinity);
        }).toThrowWithMessage(RangeError, "Invalid duration");
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Temporal.Duration).toHaveLength(0);
    });

    test("basic functionality", () => {
        const duration = new Temporal.Duration();
        expect(typeof duration).toBe("object");
        expect(duration).toBeInstanceOf(Temporal.Duration);
        expect(Object.getPrototypeOf(duration)).toBe(Temporal.Duration.prototype);
    });
});
