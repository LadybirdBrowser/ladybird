// Small collections find their keys in another way than large ones. These tests take collections through the sizes
// where that changes, in both directions.

function keysFor(count) {
    const keys = [];
    for (let i = 0; i < count; ++i) {
        switch (i % 5) {
            case 0:
                keys.push(i);
                break;
            case 1:
                keys.push("key" + i);
                break;
            case 2:
                keys.push({ i });
                break;
            case 3:
                keys.push(BigInt(i));
                break;
            case 4:
                keys.push(i + 0.5);
                break;
        }
    }
    return keys;
}

test("a Map keeps its entries while it grows and shrinks", () => {
    for (let count = 0; count <= 40; ++count) {
        const keys = keysFor(count);
        const map = new Map();
        for (let i = 0; i < count; ++i) {
            expect(map.has(keys[i])).toBeFalse();
            map.set(keys[i], i);
            expect(map.size).toBe(i + 1);
        }
        for (let i = 0; i < count; ++i) {
            expect(map.get(keys[i])).toBe(i);
        }
        expect([...map.keys()]).toEqual(keys);

        // Remove every second key, then add the keys again: they go to the end.
        for (let i = 0; i < count; i += 2) {
            expect(map.delete(keys[i])).toBeTrue();
            expect(map.delete(keys[i])).toBeFalse();
        }
        expect(map.size).toBe(Math.floor(count / 2));
        for (let i = 0; i < count; i += 2) {
            expect(map.has(keys[i])).toBeFalse();
            map.set(keys[i], -i);
        }
        const expectedKeys = [];
        for (let i = 1; i < count; i += 2) expectedKeys.push(keys[i]);
        for (let i = 0; i < count; i += 2) expectedKeys.push(keys[i]);
        expect([...map.keys()]).toEqual(expectedKeys);
        expect(map.size).toBe(count);

        for (let i = 0; i < count; ++i) {
            expect(map.delete(keys[i])).toBeTrue();
        }
        expect(map.size).toBe(0);
        expect([...map]).toEqual([]);
    }
});

test("a Set keeps its values while it grows and shrinks", () => {
    for (let count = 0; count <= 40; ++count) {
        const keys = keysFor(count);
        const set = new Set(keys);
        expect(set.size).toBe(count);
        for (const key of keys) {
            expect(set.has(key)).toBeTrue();
            set.add(key);
        }
        expect(set.size).toBe(count);
        expect([...set]).toEqual(keys);

        for (let i = count - 1; i >= 0; --i) {
            expect(set.delete(keys[i])).toBeTrue();
            expect(set.has(keys[i])).toBeFalse();
            expect(set.size).toBe(i);
        }
    }
});

test("a collection that is filled and emptied many times stays correct", () => {
    const set = new Set();
    const map = new Map();
    for (let round = 0; round < 50; ++round) {
        const count = (round * 7) % 13;
        for (let i = 0; i < count; ++i) {
            set.add("value" + i);
            map.set(i, round);
        }
        expect(set.size).toBe(count);
        expect(map.size).toBe(count);
        for (let i = 0; i < count; ++i) {
            expect(map.get(i)).toBe(round);
            expect(set.delete("value" + i)).toBeTrue();
            expect(map.delete(i)).toBeTrue();
        }
        expect(set.size).toBe(0);
        expect(map.size).toBe(0);
    }
});

test("keys with the same value are one key at every size", () => {
    for (const count of [1, 4, 8, 9, 20]) {
        const map = new Map();
        for (let i = 0; i < count; ++i) map.set("filler" + i, i);
        map.set(NaN, "nan");
        map.set(-0, "zero");
        map.set("a" + "b", "string");
        map.set(10n ** 30n, "bigint");
        const size = map.size;

        map.set(0 / 0, "nan again");
        map.set(0, "zero again");
        map.set(["a", "b"].join(""), "string again");
        map.set(1000000000000000n * 1000000000000000n, "bigint again");
        expect(map.size).toBe(size);
        expect(map.get(NaN)).toBe("nan again");
        expect(map.get(-0)).toBe("zero again");
        expect(map.get("ab")).toBe("string again");
        expect(map.get(10n ** 30n)).toBe("bigint again");
        expect(map.has(1)).toBeFalse();
        expect(map.has("1")).toBeFalse();
    }
});

test("an iterator finds its place again when the collection grows and shrinks", () => {
    const set = new Set([0, 1, 2]);
    const seen = [];
    for (const value of set) {
        seen.push(value);
        if (value < 30) {
            set.delete(value);
            set.add(value + 3);
        }
    }
    const expected = [];
    for (let i = 0; i < 33; ++i) expected.push(i);
    expect(seen).toEqual(expected);
    expect([...set]).toEqual([30, 31, 32]);
});
