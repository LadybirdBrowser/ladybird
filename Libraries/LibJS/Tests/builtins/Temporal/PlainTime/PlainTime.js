describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Temporal.PlainTime();
        }).toThrowWithMessage(
            TypeError,
            "Temporal.PlainTime constructor must be called with 'new'"
        );
    });

    test("cannot pass Infinity", () => {
        for (let i = 0; i < 6; ++i) {
            const args = Array(6).fill(0);

            args[i] = Infinity;
            expect(() => {
                new Temporal.PlainTime(...args);
            }).toThrowWithMessage(RangeError, "Invalid plain time");

            args[i] = -Infinity;
            expect(() => {
                new Temporal.PlainTime(...args);
            }).toThrowWithMessage(RangeError, "Invalid plain time");
        }
    });

    test("cannot pass invalid ISO time", () => {
        const badValues = [24, 60, 60, 1000, 1000, 1000];
        for (let i = 0; i < 6; ++i) {
            const args = [0, 0, 0, 0, 0, 0];
            args[i] = badValues[i];
            expect(() => {
                new Temporal.PlainTime(...args);
            }).toThrowWithMessage(RangeError, "Invalid plain time");
        }
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainTime).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainTime = new Temporal.PlainTime(19, 46, 32, 123, 456, 789);
        expect(typeof plainTime).toBe("object");
        expect(plainTime).toBeInstanceOf(Temporal.PlainTime);
        expect(Object.getPrototypeOf(plainTime)).toBe(Temporal.PlainTime.prototype);
    });
});
