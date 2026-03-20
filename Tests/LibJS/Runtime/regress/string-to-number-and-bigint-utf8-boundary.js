// Literal-only regression tests for inputs that previously panicked during
// numeric and bigint coercion.

// In these inputs, byte index 2 can land inside a multi-byte UTF-8 scalar.
// The old split_at(2) path could panic on this shape.
test("panic-shape inputs do not crash folded StringToNumber", () => {
    expect(+"aä").toBeNaN();
    expect(+"€").toBeNaN();
    expect(+"0💩").toBeNaN();
});

test("panic-shape inputs do not crash folded StringToBigInt", () => {
    expect("aä" == 0n).toBeFalse();
    expect("€" == 0n).toBeFalse();
    expect("0💩" == 0n).toBeFalse();
});
