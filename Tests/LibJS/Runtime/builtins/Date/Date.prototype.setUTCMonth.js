describe("errors", () => {
    test("called on non-Date object", () => {
        expect(() => {
            Date.prototype.setUTCMonth();
        }).toThrowWithMessage(TypeError, "Not an object of type Date");
    });

    test("called with non-numeric parameters", () => {
        expect(() => {
            new Date().setUTCMonth(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Date().setUTCMonth(1989, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

describe("correct behavior", () => {
    const d = new Date(2000, 2, 1);

    test("basic functionality", () => {
        d.setUTCMonth(8);
        expect(d.getUTCMonth()).toBe(8);

        d.setUTCMonth(9, 15);
        expect(d.getUTCMonth()).toBe(9);
        expect(d.getUTCDate()).toBe(15);

        d.setUTCMonth("");
        expect(d.getUTCMonth()).toBe(0);

        d.setUTCMonth("a");
        expect(d.getUTCMonth()).toBe(NaN);
    });

    test("NaN", () => {
        d.setUTCMonth(NaN);
        expect(d.getUTCMonth()).toBeNaN();
    });

    test("time clip", () => {
        d.setUTCMonth(8.65e15);
        expect(d.getUTCMonth()).toBeNaN();
    });
});

test("invalid date", () => {
    let date = new Date(NaN);
    expect(date.setUTCMonth(2)).toBeNaN();
    expect(date.getUTCMonth()).toBeNaN();
});
