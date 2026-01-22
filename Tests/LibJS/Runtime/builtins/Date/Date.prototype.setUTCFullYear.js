describe("errors", () => {
    test("called on non-Date object", () => {
        expect(() => {
            Date.prototype.setUTCFullYear();
        }).toThrowWithMessage(TypeError, "Not an object of type Date");
    });

    test("called with non-numeric parameters", () => {
        expect(() => {
            new Date().setUTCFullYear(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Date().setUTCFullYear(1989, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Date().setUTCFullYear(1989, 0, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

describe("correct behavior", () => {
    const d = new Date(2000, 2, 1);

    test("basic functionality", () => {
        d.setUTCFullYear(1989);
        expect(d.getUTCFullYear()).toBe(1989);

        d.setUTCFullYear(1990, 1);
        expect(d.getUTCFullYear()).toBe(1990);
        expect(d.getUTCMonth()).toBe(1);

        d.setUTCFullYear(1991, 2, 15);
        expect(d.getUTCFullYear()).toBe(1991);
        expect(d.getUTCMonth()).toBe(2);
        expect(d.getUTCDate()).toBe(15);

        d.setUTCFullYear("");
        expect(d.getUTCFullYear()).toBe(0);

        d.setUTCFullYear("a");
        expect(d.getUTCFullYear()).toBe(NaN);
    });

    test("NaN", () => {
        d.setUTCFullYear(NaN);
        expect(d.getUTCFullYear()).toBeNaN();
    });

    test("time clip", () => {
        d.setUTCFullYear(8.65e15);
        expect(d.getUTCFullYear()).toBeNaN();
    });
});

test("invalid date", () => {
    let date = new Date(NaN);
    date.setUTCFullYear(2022);
    expect(date.getUTCFullYear()).toBe(2022);
});
