describe("correct behavior", () => {
    test("basic functionality", () => {
        const date = new Date("2021-07-09T01:36:00Z");
        const instant = date.toTemporalInstant();
        expect(instant.epochSeconds).toBe(1625794560);
    });
});

describe("errors", () => {
    test("this value must be a Date object", () => {
        expect(() => {
            Date.prototype.toTemporalInstant.call(123);
        }).toThrowWithMessage(TypeError, "Not an object of type Date");
    });
});
