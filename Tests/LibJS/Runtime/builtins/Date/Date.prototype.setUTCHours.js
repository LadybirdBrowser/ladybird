describe("errors", () => {
    test("called on non-Date object", () => {
        expect(() => {
            Date.prototype.setUTCHours();
        }).toThrowWithMessage(TypeError, "Not an object of type Date");
    });

    test("called with non-numeric parameters", () => {
        expect(() => {
            new Date().setUTCHours(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Date().setUTCHours(8, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Date().setUTCHours(8, 9, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Date().setUTCHours(8, 9, 10, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

describe("correct behavior", () => {
    const d = new Date(2000, 2, 1);

    test("basic functionality", () => {
        d.setUTCHours(8);
        expect(d.getUTCHours()).toBe(8);

        d.setUTCHours(9, 15);
        expect(d.getUTCHours()).toBe(9);
        expect(d.getUTCMinutes()).toBe(15);

        d.setUTCHours(10, 25, 35);
        expect(d.getUTCHours()).toBe(10);
        expect(d.getUTCMinutes()).toBe(25);
        expect(d.getUTCSeconds()).toBe(35);

        d.setUTCHours(11, 35, 45, 789);
        expect(d.getUTCHours()).toBe(11);
        expect(d.getUTCMinutes()).toBe(35);
        expect(d.getUTCSeconds()).toBe(45);
        expect(d.getUTCMilliseconds()).toBe(789);

        d.setUTCHours("");
        expect(d.getUTCHours()).toBe(0);

        d.setUTCHours("a");
        expect(d.getUTCHours()).toBe(NaN);
    });

    test("NaN", () => {
        d.setUTCHours(NaN);
        expect(d.getUTCHours()).toBeNaN();
    });

    test("time clip", () => {
        d.setUTCHours(8.65e15);
        expect(d.getUTCHours()).toBeNaN();
    });
});

test("invalid date", () => {
    let date = new Date(NaN);
    expect(date.setUTCHours(2)).toBeNaN();
    expect(date.getUTCHours()).toBeNaN();
});
