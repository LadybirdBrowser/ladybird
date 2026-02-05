describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.caseFirst;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        expect(new Intl.Locale("en").caseFirst).toBeUndefined();
        expect(new Intl.Locale("en-u-kf-upper").caseFirst).toBe("upper");
        expect(new Intl.Locale("en", { caseFirst: "lower" }).caseFirst).toBe("lower");
        expect(new Intl.Locale("en-u-kf-upper", { caseFirst: "false" }).caseFirst).toBe("false");
    });
});
