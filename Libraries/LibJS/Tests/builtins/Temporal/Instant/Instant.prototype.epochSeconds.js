describe("correct behavior", () => {
    test("basic functionality", () => {
        expect(new Temporal.Instant(0n).epochSeconds).toBe(0);
        expect(new Temporal.Instant(1n).epochSeconds).toBe(0);
        expect(new Temporal.Instant(999_999_999n).epochSeconds).toBe(0);
        expect(new Temporal.Instant(1_000_000_000n).epochSeconds).toBe(1);
        expect(new Temporal.Instant(1_500_000_000n).epochSeconds).toBe(1);
        expect(new Temporal.Instant(1_999_999_999n).epochSeconds).toBe(1);
        expect(new Temporal.Instant(2_000_000_000n).epochSeconds).toBe(2);
        expect(new Temporal.Instant(8_640_000_000_000_000_000_000n).epochSeconds).toBe(
            8_640_000_000_000
        );

        expect(new Temporal.Instant(-0n).epochSeconds).toBe(0);
        expect(new Temporal.Instant(-1n).epochSeconds).toBe(0);
        expect(new Temporal.Instant(-999_999_999n).epochSeconds).toBe(0);
        expect(new Temporal.Instant(-1_000_000_000n).epochSeconds).toBe(-1);
        expect(new Temporal.Instant(-1_500_000_000n).epochSeconds).toBe(-1);
        expect(new Temporal.Instant(-1_999_999_999n).epochSeconds).toBe(-1);
        expect(new Temporal.Instant(-2_000_000_000n).epochSeconds).toBe(-2);
        expect(new Temporal.Instant(-8_640_000_000_000_000_000_000n).epochSeconds).toBe(
            -8_640_000_000_000
        );
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Instant object", () => {
        expect(() => {
            Reflect.get(Temporal.Instant.prototype, "epochSeconds", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Instant");
    });
});
