describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Temporal.Calendar();
        }).toThrowWithMessage(TypeError, "Temporal.Calendar constructor must be called with 'new'");
    });

    test("argument must be coercible to string", () => {
        expect(() => {
            new Temporal.Calendar({
                toString() {
                    throw new Error();
                },
            });
        }).toThrow(Error);
    });

    test("invalid calendar identifier", () => {
        expect(() => {
            new Temporal.Calendar("foo");
        }).toThrowWithMessage(RangeError, "Invalid calendar identifier 'foo'");
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Calendar).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(calendar.id).toBe("iso8601");
        expect(typeof calendar).toBe("object");
        expect(calendar).toBeInstanceOf(Temporal.Calendar);
        expect(Object.getPrototypeOf(calendar)).toBe(Temporal.Calendar.prototype);
    });
});
