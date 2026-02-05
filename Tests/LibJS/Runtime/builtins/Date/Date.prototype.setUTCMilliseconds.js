describe("errors", () => {
    test("called on non-Date object", () => {
        expect(() => {
            Date.prototype.setUTCMilliseconds();
        }).toThrowWithMessage(TypeError, "Not an object of type Date");
    });

    test("called with non-numeric parameters", () => {
        expect(() => {
            new Date().setUTCMilliseconds(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

describe("correct behavior", () => {
    const d = new Date(2000, 2, 1);

    test("basic functionality", () => {
        d.setUTCMilliseconds(8);
        expect(d.getUTCMilliseconds()).toBe(8);

        d.setUTCMilliseconds("");
        expect(d.getUTCMilliseconds()).toBe(0);

        d.setUTCMilliseconds("a");
        expect(d.getUTCMilliseconds()).toBe(NaN);
    });

    test("NaN", () => {
        d.setUTCMilliseconds(NaN);
        expect(d.getUTCMilliseconds()).toBeNaN();
    });

    test("time clip", () => {
        d.setUTCMilliseconds(8.65e15);
        expect(d.getUTCMilliseconds()).toBeNaN();
    });
});

test("invalid date", () => {
    let date = new Date(NaN);
    expect(date.setUTCMilliseconds(2)).toBeNaN();
    expect(date.getUTCMilliseconds()).toBeNaN();
});
