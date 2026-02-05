describe("correct behavior", () => {
    test("basic functionality", () => {
        expect(new Temporal.Instant(0n).epochNanoseconds).toBe(0n);
        expect(new Temporal.Instant(1n).epochNanoseconds).toBe(1n);
        expect(new Temporal.Instant(999n).epochNanoseconds).toBe(999n);
        expect(new Temporal.Instant(8_640_000_000_000_000_000_000n).epochNanoseconds).toBe(
            8_640_000_000_000_000_000_000n
        );

        expect(new Temporal.Instant(-0n).epochNanoseconds).toBe(-0n);
        expect(new Temporal.Instant(-1n).epochNanoseconds).toBe(-1n);
        expect(new Temporal.Instant(-999n).epochNanoseconds).toBe(-999n);
        expect(new Temporal.Instant(-8_640_000_000_000_000_000_000n).epochNanoseconds).toBe(
            -8_640_000_000_000_000_000_000n
        );
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Instant object", () => {
        expect(() => {
            Reflect.get(Temporal.Instant.prototype, "epochNanoseconds", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Instant");
    });
});
