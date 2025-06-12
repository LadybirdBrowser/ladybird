describe("errors", () => {
    test("called on non-Date object", () => {
        expect(() => {
            Date.prototype.setUTCMinutes();
        }).toThrowWithMessage(TypeError, "Not an object of type Date");
    });

    test("called with non-numeric parameters", () => {
        expect(() => {
            new Date().setUTCMinutes(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Date().setUTCMinutes(1989, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

describe("correct behavior", () => {
    const d = new Date(2000, 2, 1);

    test("basic functionality", () => {
        d.setUTCMinutes(8);
        expect(d.getUTCMinutes()).toBe(8);

        d.setUTCMinutes(9, 15);
        expect(d.getUTCMinutes()).toBe(9);
        expect(d.getUTCSeconds()).toBe(15);

        d.setUTCMinutes(55, 44, 456);
        expect(d.getUTCMinutes()).toBe(55);
        expect(d.getUTCSeconds()).toBe(44);
        expect(d.getUTCMilliseconds()).toBe(456);

        d.setUTCMinutes("");
        expect(d.getUTCMinutes()).toBe(0);

        d.setUTCMinutes("a");
        expect(d.getUTCMinutes()).toBe(NaN);
    });

    test("NaN", () => {
        d.setUTCMinutes(NaN);
        expect(d.getUTCMinutes()).toBeNaN();
    });

    test("time clip", () => {
        d.setUTCMinutes(8.65e15);
        expect(d.getUTCMinutes()).toBeNaN();
    });
});

test("invalid date", () => {
    let date = new Date(NaN);
    expect(date.setUTCMinutes(2)).toBeNaN();
    expect(date.getUTCMinutes()).toBeNaN();
});
