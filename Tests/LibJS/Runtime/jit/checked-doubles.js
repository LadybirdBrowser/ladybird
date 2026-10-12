test("compiled comparisons of doubles that held int32 values stay exact", () => {
    const keys = new Float64Array([1, 2, 3, 4]);
    const countBelow = new Function(
        "keys",
        "limit",
        `let count = 0;
        for (let i = 0; i < keys.length; ++i) {
            if (keys[i] < limit) count = (count + 1) | 0;
        }
        return count;`
    );
    jit.prepare(countBelow);
    for (let k = 0; k < 10; ++k) countBelow(keys, k);
    expect(jit.compile(countBelow)).toBe(jit.enabled);
    expect(countBelow(keys, 2.5)).toBe(2);
    keys.set([0.5, -0, NaN, 3.25]);
    expect(countBelow(keys, 0)).toBe(0);
    expect(countBelow(keys, 1)).toBe(2);
    expect(countBelow(keys, 3.5)).toBe(3);
    expect(countBelow(keys, Infinity)).toBe(3);
});

test("compiled arithmetic on doubles that held int32 values stays exact", () => {
    const values = new Float64Array([1, 2, 3, 4]);
    const sum = new Function(
        "values",
        `let total = 0;
        for (let i = 0; i < values.length; ++i) total += values[i];
        return total;`
    );
    jit.prepare(sum);
    for (let k = 0; k < 10; ++k) sum(values);
    expect(jit.compile(sum)).toBe(jit.enabled);
    expect(sum(values)).toBe(10);
    values.set([0.5, 0.25, -0, 1e300]);
    expect(sum(values)).toBe(0.75 + 1e300);
    values.set([NaN, 1, 2, 3]);
    expect(sum(values)).toBeNaN();
});

test("unused int32 arithmetic keeps converting operands with effects", () => {
    const add = new Function("a", "b", "const sum = a + b; return 1;");
    jit.prepare(add);
    for (let k = 0; k < 10; ++k) add(k, 2);
    jit.compile(add);
    let calls = 0;
    expect(add({ valueOf: () => ++calls }, 1)).toBe(1);
    expect(calls).toBe(1);
    expect(() => add(Symbol(), 1)).toThrow(TypeError);
    expect(() => add(1n, 1)).toThrow(TypeError);
});

test("doubles that are both compared and used as indices keep their checks", () => {
    const lookup = new Function(
        "keys",
        "table",
        "limit",
        `let total = 0;
        for (let i = 0; i < keys.length; ++i) {
            if (keys[i] < limit) total += table[keys[i]];
        }
        return total;`
    );
    const keys = new Float64Array([0, 1, 2, 3]);
    const table = [10, 20, 30, 40];
    jit.prepare(lookup);
    for (let k = 0; k < 10; ++k) lookup(keys, table, 4);
    jit.compile(lookup);
    expect(lookup(keys, table, 4)).toBe(100);
    // NB: Keys that are no int32 values index no elements.
    keys.set([1, 1, 2.5, 3]);
    expect(lookup(keys, table, 2)).toBe(40);
    expect(lookup(keys, table, 4)).toBeNaN();
});
