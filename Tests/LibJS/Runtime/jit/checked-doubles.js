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

test("compiled identities of doubles that held int32 values convert them", () => {
    const midpoint = new Function("low", "high", "return ((low + high) / 2) | 0;");
    const plusZero = new Function("values", "return values[0] + 0;");
    const shifted = new Function("values", "return (values[0] << 0) + (values[0] >> 0) + (values[0] ^ 0);");
    const values = new Float64Array([4]);
    for (const f of [midpoint, plusZero, shifted]) jit.prepare(f);
    for (let k = 0; k < 10; ++k) {
        midpoint(k, k + 2);
        plusZero(values);
        shifted(values);
    }
    for (const f of [midpoint, plusZero, shifted]) expect(jit.compile(f)).toBe(jit.enabled);
    expect(midpoint(1, 2)).toBe(1);
    expect(midpoint(-1, -2)).toBe(-1);
    expect(midpoint(0, NaN)).toBe(0);
    expect(midpoint(2 ** 32, 2 ** 32 + 6)).toBe(3);
    values[0] = -0;
    expect(Object.is(plusZero(values), 0)).toBeTrue();
    values[0] = 2.5;
    expect(plusZero(values)).toBe(2.5);
    expect(shifted(values)).toBe(6);
    values[0] = -(2 ** 31) - 1.5;
    expect(shifted(values)).toBe(3 * (2 ** 31 - 1));
});

test("folds that give doubles checked to be int32 values int32 users keep the checks", () => {
    const sum = new Function(
        "a",
        "b",
        "count",
        "start",
        `let y = +(a * b);
        let s = start;
        for (let i = 0; i < count; i++) {
            let t = y;
            if (i > 1000) t = y;
            y = t;
            s = s + y;
        }
        return s;`
    );
    jit.prepare(sum);
    for (let k = 0; k < 10; ++k) {
        sum(1.5, 2, 3, 0);
        sum(1.5, 2, 2, "a");
    }
    expect(jit.compile(sum)).toBe(jit.enabled);
    expect(sum(1.5, 2, 3, 0)).toBe(9);
    expect(sum(1.5, 2, 2, "a")).toBe("a33");
    expect(sum(1.25, 2, 2, 1)).toBe(6);
    expect(sum(1.25, 2, 2, "b")).toBe("b2.52.5");
});
