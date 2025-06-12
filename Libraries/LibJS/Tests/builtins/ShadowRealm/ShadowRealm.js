describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            ShadowRealm();
        }).toThrowWithMessage(TypeError, "ShadowRealm constructor must be called with 'new'");
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(ShadowRealm).toHaveLength(0);
    });

    test("basic functionality", () => {
        const shadowRealm = new ShadowRealm();
        expect(typeof shadowRealm).toBe("object");
        expect(shadowRealm).toBeInstanceOf(ShadowRealm);
        expect(Object.getPrototypeOf(shadowRealm)).toBe(ShadowRealm.prototype);
    });
});
