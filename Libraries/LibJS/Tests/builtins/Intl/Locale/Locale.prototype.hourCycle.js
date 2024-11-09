describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.hourCycle;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        expect(new Intl.Locale("en").hourCycle).toBeUndefined();
        expect(new Intl.Locale("en-u-hc-h11").hourCycle).toBe("h11");
        expect(new Intl.Locale("en", { hourCycle: "h12" }).hourCycle).toBe("h12");
        expect(new Intl.Locale("en-u-hc-h23", { hourCycle: "h24" }).hourCycle).toBe("h24");
    });
});
