describe("correct behavior", () => {
    test("basic functionality", () => {
        expect(new Temporal.Instant(0n).epochMicroseconds).toBe(0n);
        expect(new Temporal.Instant(1n).epochMicroseconds).toBe(0n);
        expect(new Temporal.Instant(999n).epochMicroseconds).toBe(0n);
        expect(new Temporal.Instant(1_000n).epochMicroseconds).toBe(1n);
        expect(new Temporal.Instant(1_500n).epochMicroseconds).toBe(1n);
        expect(new Temporal.Instant(1_999n).epochMicroseconds).toBe(1n);
        expect(new Temporal.Instant(2_000n).epochMicroseconds).toBe(2n);
        expect(new Temporal.Instant(8_640_000_000_000_000_000_000n).epochMicroseconds).toBe(
            8_640_000_000_000_000_000n
        );

        expect(new Temporal.Instant(-0n).epochMicroseconds).toBe(-0n);
        expect(new Temporal.Instant(-1n).epochMicroseconds).toBe(-0n);
        expect(new Temporal.Instant(-999n).epochMicroseconds).toBe(-0n);
        expect(new Temporal.Instant(-1_000n).epochMicroseconds).toBe(-1n);
        expect(new Temporal.Instant(-1_500n).epochMicroseconds).toBe(-1n);
        expect(new Temporal.Instant(-1_999n).epochMicroseconds).toBe(-1n);
        expect(new Temporal.Instant(-2_000n).epochMicroseconds).toBe(-2n);
        expect(new Temporal.Instant(-8_640_000_000_000_000_000_000n).epochMicroseconds).toBe(
            -8_640_000_000_000_000_000n
        );
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Instant object", () => {
        expect(() => {
            Reflect.get(Temporal.Instant.prototype, "epochMicroseconds", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Instant");
    });
});
