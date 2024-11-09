test("constructor properties", () => {
    expect(FinalizationRegistry).toHaveLength(1);
    expect(FinalizationRegistry.name).toBe("FinalizationRegistry");
});

describe("errors", () => {
    test("invalid callbacks", () => {
        [-100, Infinity, NaN, 152n, undefined].forEach(value => {
            expect(() => {
                new FinalizationRegistry(value);
            }).toThrowWithMessage(TypeError, "is not a function");
        });
    });
    test("called without new", () => {
        expect(() => {
            FinalizationRegistry();
        }).toThrowWithMessage(
            TypeError,
            "FinalizationRegistry constructor must be called with 'new'"
        );
    });
});

describe("normal behavior", () => {
    test("typeof", () => {
        expect(typeof new FinalizationRegistry(() => {})).toBe("object");
    });

    test("constructor with single callback argument", () => {
        var a = new FinalizationRegistry(() => {});
        expect(a instanceof FinalizationRegistry).toBeTrue();
    });
});
