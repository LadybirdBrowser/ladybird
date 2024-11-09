describe("errors", () => {
    test("called on non-Date object", () => {
        expect(() => {
            Date.prototype.getTimezoneOffset();
        }).toThrowWithMessage(TypeError, "Not an object of type Date");
    });
});

describe("correct behavior", () => {
    test("NaN", () => {
        const d = new Date(NaN);
        expect(d.getTimezoneOffset()).toBeNaN();
    });

    test("time clip", () => {
        const d = new Date(-8.65e15);
        expect(d.getTimezoneOffset()).toBeNaN();
    });

    test("basic functionality", () => {
        // Exact return values from getTimezoneOffset depend on the time zone of the host machine.
        // So we can't test exact values, but that value should not change here.
        const d0 = new Date(Date.parse("1989-01-23T14:30-00:00"));
        const d1 = new Date(Date.parse("1989-01-23T14:30-05:00"));

        const offset0 = d0.getTimezoneOffset();
        const offset1 = d1.getTimezoneOffset();
        expect(offset0).toBe(offset1);
    });
});
