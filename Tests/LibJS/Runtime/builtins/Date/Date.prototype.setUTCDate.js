describe("errors", () => {
    test("called on non-Date object", () => {
        expect(() => {
            Date.prototype.setUTCDate();
        }).toThrowWithMessage(TypeError, "Not an object of type Date");
    });

    test("called with non-numeric parameters", () => {
        expect(() => {
            new Date().setUTCDate(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

describe("correct behavior", () => {
    const d = new Date(2000, 2, 1);

    test("basic functionality", () => {
        d.setUTCDate(8);
        expect(d.getUTCDate()).toBe(8);

        d.setUTCDate("a");
        expect(d.getUTCDate()).toBe(NaN);
    });

    test("NaN", () => {
        d.setUTCDate(NaN);
        expect(d.getUTCDate()).toBeNaN();
    });

    test("time clip", () => {
        d.setUTCDate(8.65e15);
        expect(d.getUTCDate()).toBeNaN();
    });
});

test("invalid date", () => {
    let date = new Date(NaN);
    expect(date.setUTCDate(15)).toBeNaN();
    expect(date.getUTCDate()).toBeNaN();
});
