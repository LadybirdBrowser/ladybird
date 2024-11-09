describe("correct behavior", () => {
    test("basic functionality", () => {
        expect(new Temporal.Instant(0n).epochMilliseconds).toBe(0);
        expect(new Temporal.Instant(1n).epochMilliseconds).toBe(0);
        expect(new Temporal.Instant(999_999n).epochMilliseconds).toBe(0);
        expect(new Temporal.Instant(1_000_000n).epochMilliseconds).toBe(1);
        expect(new Temporal.Instant(1_500_000n).epochMilliseconds).toBe(1);
        expect(new Temporal.Instant(1_999_999n).epochMilliseconds).toBe(1);
        expect(new Temporal.Instant(2_000_000n).epochMilliseconds).toBe(2);
        expect(new Temporal.Instant(8_640_000_000_000_000_000_000n).epochMilliseconds).toBe(
            8_640_000_000_000_000
        );

        expect(new Temporal.Instant(-0n).epochMilliseconds).toBe(0);
        expect(new Temporal.Instant(-1n).epochMilliseconds).toBe(0);
        expect(new Temporal.Instant(-999_999n).epochMilliseconds).toBe(0);
        expect(new Temporal.Instant(-1_000_000n).epochMilliseconds).toBe(-1);
        expect(new Temporal.Instant(-1_500_000n).epochMilliseconds).toBe(-1);
        expect(new Temporal.Instant(-1_999_999n).epochMilliseconds).toBe(-1);
        expect(new Temporal.Instant(-2_000_000n).epochMilliseconds).toBe(-2);
        expect(new Temporal.Instant(-8_640_000_000_000_000_000_000n).epochMilliseconds).toBe(
            -8_640_000_000_000_000
        );
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Instant object", () => {
        expect(() => {
            Reflect.get(Temporal.Instant.prototype, "epochMilliseconds", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Instant");
    });
});
