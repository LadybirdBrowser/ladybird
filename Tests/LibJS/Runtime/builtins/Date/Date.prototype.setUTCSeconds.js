describe("errors", () => {
    test("called on non-Date object", () => {
        expect(() => {
            Date.prototype.setUTCSeconds();
        }).toThrowWithMessage(TypeError, "Not an object of type Date");
    });

    test("called with non-numeric parameters", () => {
        expect(() => {
            new Date().setUTCSeconds(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Date().setUTCSeconds(1989, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

describe("correct behavior", () => {
    const d = new Date(2000, 2, 1);

    test("basic functionality", () => {
        d.setUTCSeconds(50);
        expect(d.getUTCSeconds()).toBe(50);

        d.setUTCSeconds(50, 600);
        expect(d.getUTCSeconds()).toBe(50);
        expect(d.getUTCMilliseconds()).toBe(600);

        d.setUTCSeconds("");
        expect(d.getUTCSeconds()).toBe(0);

        d.setUTCSeconds("a");
        expect(d.getUTCSeconds()).toBe(NaN);
    });

    test("NaN", () => {
        d.setUTCSeconds(NaN);
        expect(d.getUTCSeconds()).toBeNaN();
    });

    test("time clip", () => {
        d.setUTCSeconds(8.65e15);
        expect(d.getUTCSeconds()).toBeNaN();
    });
});

test("invalid date", () => {
    let date = new Date(NaN);
    expect(date.setUTCSeconds(2)).toBeNaN();
    expect(date.getUTCSeconds()).toBeNaN();
});
