// Literal-only regression tests that pin non-decimal prefix behavior in
// numeric and bigint coercion expressions.

function expectLooseNumberEquality(value, target, expected) {
    expect(value == target).toBe(expected);
    expect(value != target).toBe(!expected);
    expect(target == value).toBe(expected);
    expect(target != value).toBe(!expected);
}

function expectLooseBigIntEquality(value, target, expected) {
    expect(value == target).toBe(expected);
    expect(value != target).toBe(!expected);
    expect(target == value).toBe(expected);
    expect(target != value).toBe(!expected);
}

function expectLooseEquality(value, numberTarget, bigintTarget, expected) {
    expectLooseNumberEquality(value, numberTarget, expected);
    expectLooseBigIntEquality(value, bigintTarget, expected);
}

test("naked non-decimal prefixes stay invalid in folded number contexts", () => {
    for (const value of ["0x", "0o", "0b"]) expect(+value).toBeNaN();
});

test("uppercase naked prefixes stay invalid in folded number contexts", () => {
    for (const value of ["0X", "0O", "0B"]) expect(+value).toBeNaN();
});

test("uppercase prefixed literals stay valid in folded loose equality", () => {
    for (const [value, numberTarget, bigintTarget] of [
        ["0X10", 16, 16n],
        ["0O10", 8, 8n],
        ["0B10", 2, 2n],
    ]) {
        expectLooseEquality(value, numberTarget, bigintTarget, true);
    }
});

test("trimmed uppercase prefixed literals stay valid in folded loose equality", () => {
    for (const [value, numberTarget, bigintTarget] of [
        ["  0X10  ", 16, 16n],
        ["  0O10  ", 8, 8n],
        ["  0B10  ", 2, 2n],
    ]) {
        expectLooseEquality(value, numberTarget, bigintTarget, true);
    }
});

test("valid non-decimal literals stay valid in folded number contexts", () => {
    expect(+"0x10").toBe(16);
    expect(+"0o10").toBe(8);
    expect(+"0b10").toBe(2);

    expect(+"  0X10  ").toBe(16);
    expect(+"  0O10  ").toBe(8);
    expect(+"  0B10  ").toBe(2);
});

test("non-JS suffix syntax stays invalid in folded number contexts", () => {
    for (const value of ["0x+1", "0x1_0", "0b+1", "0b1_0", "0o+7", "0o1_0"]) expect(+value).toBeNaN();
});

test("naked non-decimal prefixes stay invalid in folded bigint relational contexts", () => {
    for (const value of ["0x", "0o", "0b"]) {
        expect(value < 1n).toBeFalse();
        expect(1n < value).toBeFalse();
    }
});

test("naked prefixes stay invalid in folded loose equality", () => {
    for (const value of ["0x", "0o", "0b"]) expectLooseEquality(value, 0, 0n, false);
});

test("invalid prefixed digits stay invalid in folded loose equality", () => {
    for (const value of ["0xg", "0b2", "0o8"]) expectLooseEquality(value, 0, 0n, false);
});

test("non-JS suffix syntax stays invalid in folded loose equality", () => {
    for (const [value, numberTarget, bigintTarget] of [
        ["0x+1", 1, 1n],
        ["0x1_0", 16, 16n],
        ["0b+1", 1, 1n],
        ["0b1_0", 2, 2n],
        ["0o+7", 7, 7n],
        ["0o1_0", 8, 8n],
    ]) {
        expectLooseEquality(value, numberTarget, bigintTarget, false);
    }
});

test("valid prefixed digits stay valid in folded loose equality", () => {
    for (const [value, numberTarget, bigintTarget] of [
        ["0x10", 16, 16n],
        ["0o10", 8, 8n],
        ["0b10", 2, 2n],
    ]) {
        expectLooseEquality(value, numberTarget, bigintTarget, true);
    }
});
