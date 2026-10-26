test("computed assignment and update keys are converted only once", () => {
    for (const expression of [
        "object[key] = 2",
        "object[key] += 2",
        "object[key] -= 2",
        "object[key] *= 2",
        "object[key] /= 2",
        "object[key] %= 2",
        "object[key] **= 2",
        "object[key] <<= 2",
        "object[key] >>= 2",
        "object[key] >>>= 2",
        "object[key] &= 2",
        "object[key] |= 2",
        "object[key] ^= 2",
        "object[key] &&= 2",
        "object[key] ||= 2",
        "object[key] ??= 2",
        "++object[key]",
        "object[key]++",
        "--object[key]",
        "object[key]--",
    ]) {
        let conversions = 0;
        const key = {
            [Symbol.toPrimitive](hint) {
                expect(hint).toBe("string");
                conversions++;
                return "x";
            },
        };
        const initial = expression.includes("||=") ? false : expression.includes("??=") ? null : 1;
        const run = new Function("object", "key", expression);
        run({ x: initial }, key);
        expect(conversions).toBe(1);
    }
});

test("computed keys are converted before the right-hand side", () => {
    const log = [];
    const key = {
        toString() {
            log.push("key");
            return "x";
        },
    };
    const object = { x: 1 };
    object[key] += (log.push("rhs"), 2);
    expect(log).toEqual(["key", "rhs"]);
    expect(object.x).toBe(3);
});

test("nullish bases are rejected before key conversion or the right-hand side", () => {
    for (const expression of ["base[key] += rhs()", "base[key]++"]) {
        for (const base of [null, undefined]) {
            const key = {
                toString() {
                    throw new Error("The property key must not be converted");
                },
            };
            const rhs = () => {
                throw new Error("The right-hand side must not be evaluated");
            };
            const run = new Function("base", "key", "rhs", expression);
            expect(() => run(base, key, rhs)).toThrow(TypeError);
        }
    }
});

test("member bases survive changes made while evaluating the property", () => {
    for (const expression of ["base[(base = other, 'x')]++", "base[(base = other, 'x')] += 2"]) {
        const object = { x: 1 };
        const other = { x: 100 };
        new Function("base", "other", expression)(object, other);
        expect(object.x).toBe(expression.endsWith("++") ? 2 : 3);
        expect(other.x).toBe(100);
    }
});

test("member bases survive changes made by the right-hand side", () => {
    for (const expression of ["base.x += (base = other, 2)", "base['x'] += (base = other, 2)"]) {
        const object = { x: 1 };
        const other = { x: 100 };
        new Function("base", "other", expression)(object, other);
        expect(object.x).toBe(3);
        expect(other.x).toBe(100);
    }
});

test("super references retain the base resolved before key conversion", () => {
    for (const [expression, expectedResult, expectedProperty] of [
        ["super[key] = 2", 2, 2],
        ["super[key] += 2", 102, 102],
        ["super[key]++", 100, 101],
        ["super[key] ||= 2", 100, 1],
    ]) {
        const original = { x: 100 };
        const replacement = { x: 1 };
        const object = new Function(`return { run(key) { return ${expression}; } };`)();
        Object.setPrototypeOf(object, original);
        let conversions = 0;
        const key = {
            toString() {
                conversions++;
                Object.setPrototypeOf(object, replacement);
                return "x";
            },
        };
        const result = object.run(key);
        expect(result).toBe(expectedResult);
        expect(conversions).toBe(1);
        expect(object.x).toBe(expectedProperty);
        expect(original.x).toBe(100);
    }
});
