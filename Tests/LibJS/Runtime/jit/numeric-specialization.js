test("numeric operations keep mixed numbers, signed zero, and non-finite values", () => {
    const expressions = [
        "a + b",
        "a - b",
        "a * b",
        "a / b",
        "a & b",
        "a | b",
        "a ^ b",
        "a << b",
        "a >> b",
        "a >>> b",
        "a < b",
        "a <= b",
        "a > b",
        "a >= b",
        "a === b",
        "a !== b",
        "a == b",
        "a != b",
        "+a",
        "-a",
        "~a",
        "a | 0",
    ];
    const values = [
        -Infinity,
        -4294967297.5,
        -2147483648,
        -1.5,
        -0,
        0,
        0.5,
        1,
        2.5,
        2147483647,
        2147483648,
        4294967295,
        1e300,
        Infinity,
        NaN,
    ];
    for (const expression of expressions) {
        const compiled = new Function("a", "b", `return ${expression};`);
        const reference = new Function("a", "b", `return ${expression};`);
        jit.neverCompile(reference);
        jit.prepare(compiled);
        for (let i = 0; i < 10; ++i) compiled(i + 0.5, i + 1.25);
        expect(jit.compile(compiled)).toBe(jit.enabled);
        for (const a of values) {
            for (const b of values) expect(compiled(a, b)).toBe(reference(a, b));
        }
        expect(jit.exitCount(compiled)).toBe(0);
        for (const [a, b] of [
            ["2", "3"],
            [true, false],
            [null, undefined],
            [3n, 1n],
        ]) {
            if (expression === "+a" || expression === "a >>> b" || expression === "a | 0") {
                if (typeof a === "bigint") {
                    expect(() => compiled(a, b)).toThrow(TypeError);
                    continue;
                }
            }
            expect(compiled(a, b)).toBe(reference(a, b));
        }
    }
});

test("number guards preserve coercion order and exceptions", () => {
    const operations = ["a + b", "a - b", "a * b", "a / b", "a & b", "a < b"];
    for (const expression of operations) {
        const compiled = new Function("a", "b", `return ${expression};`);
        jit.prepare(compiled);
        compiled(1.5, 2.5);
        jit.compile(compiled);
        const calls = [];
        const a = {
            valueOf() {
                calls.push("a");
                return 1.5;
            },
        };
        const b = {
            valueOf() {
                calls.push("b");
                return 2.5;
            },
        };
        compiled(a, b);
        expect(calls).toEqual(["a", "b"]);
        calls.length = 0;
        const throwing = {
            valueOf() {
                calls.push("throw");
                throw new Error("coercion");
            },
        };
        expect(() => compiled(throwing, b)).toThrowWithMessage(Error, "coercion");
        expect(calls).toEqual(["throw"]);
    }
});

test("numeric loop entries and updates preserve values on type changes", () => {
    const sum = new Function(
        "start",
        "let value = start; for (let i = 0; i < 20; ++i) value += i * 0.25; return value;"
    );
    const update = new Function("start", "let value = start; const old = value++; return [old, --value];");
    for (const f of [sum, update]) {
        jit.prepare(f);
        for (let i = 0; i < 10; ++i) f(i + 0.5);
        expect(jit.compile(f)).toBe(jit.enabled);
    }
    expect(sum(0.5)).toBe(48);
    expect(sum(0)).toBe(47.5);
    expect(update(-0)).toEqual([-0, 0]);
    expect(update(Infinity)).toEqual([Infinity, Infinity]);
    expect(update(NaN)).toEqual([NaN, NaN]);
    expect(update("2")).toEqual([2, 2]);
    expect(update(2n)).toEqual([2n, 2n]);
    expect(sum("x")).toBe("x00.250.50.7511.251.51.7522.252.52.7533.253.53.7544.254.54.75");
});

test("number guards restore loop state when a later iteration needs coercion", () => {
    const sum = new Function(
        "values",
        "let total = 0.5; for (let i = 0; i < values.length; ++i) total += values[i]; return total;"
    );
    const values = new Array(20).fill(1.25);
    jit.prepare(sum);
    for (let i = 0; i < 10; ++i) expect(sum(values)).toBe(25.5);
    expect(jit.compile(sum)).toBe(jit.enabled);
    let calls = 0;
    values[13] = {
        valueOf() {
            ++calls;
            return 2.5;
        },
    };
    expect(sum(values)).toBe(26.75);
    expect(calls).toBe(1);
    values[13] = "x";
    expect(sum(values)).toBe("16.75x1.251.251.251.251.251.25");
});
