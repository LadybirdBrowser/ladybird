function compiled(f, ...warmupArguments) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...warmupArguments);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

test("elements read twice give the same value", () => {
    const digits = compiled(new Function("a", "i", "return (a[i] & 0x3fff) + (a[i] >> 14);"), [1 << 20, 2, 3], 0);
    expect(digits([(5 << 14) | 7, 1], 0)).toBe(12);
    expect(digits([1, (3 << 14) | 2], 1)).toBe(5);
});

test("stores between reads of an element are seen", () => {
    const update = compiled(
        new Function(
            "a",
            "b",
            "i",
            `const before = a[i];
            b[i] = before + 10;
            return before * 100 + a[i];`
        ),
        [1, 2],
        [3, 4],
        0
    );
    expect(update([1, 2], [3, 4], 1)).toBe(202);
    const shared = [5, 6];
    expect(update(shared, shared, 0)).toBe(515);
    expect(shared).toEqual([15, 6]);
});

test("stores through typed arrays sharing a buffer are seen", () => {
    const read = compiled(
        new Function(
            "bytes",
            "words",
            `const before = bytes[0];
            words[0] = 0x01020304;
            return before * 1000 + bytes[0];`
        ),
        new Uint8Array(4),
        new Uint32Array(4)
    );
    const buffer = new ArrayBuffer(16);
    const bytes = new Uint8Array(buffer);
    bytes[0] = 7;
    expect(read(bytes, new Uint32Array(buffer))).toBe(7004);
});

test("calls between reads of an element are seen", () => {
    const read = compiled(
        new Function(
            "a",
            "f",
            `const before = a[0];
            f(a);
            return before * 100 + a[0];`
        ),
        [1],
        () => {}
    );
    expect(
        read([2], a => {
            a[0] = 3;
        })
    ).toBe(203);
});

test("holes read twice exit for both reads", () => {
    const read = compiled(new Function("a", "i", "return [a[i], a[i]];"), [1, 2], 0);
    expect(read([1, , 3], 1)).toEqual([undefined, undefined]);
    Array.prototype[1] = "inherited";
    try {
        expect(read([1, , 3], 1)).toEqual(["inherited", "inherited"]);
    } finally {
        delete Array.prototype[1];
    }
});
