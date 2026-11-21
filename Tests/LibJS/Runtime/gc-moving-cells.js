// The objects below are only reachable through the properties of `holder`, which the collector can rewrite, so a
// collection that moves cells moves them.
const holder = {
    small: { a: 1, b: "two" },
    large: {},
    array: [1, 2, 3, { x: 4 }],
    hashed: {},
    mapKey: {},
    weakMapKey: {},
    counter: (() => {
        let count = 0;
        return () => ++count;
    })(),
};
holder.counter();
holder.rope = "x".repeat(30) + "y".repeat(30);
holder.substring = holder.rope.substring(25, 35);
holder.flat = String(holder.rope.length) + "!";
for (let i = 0; i < 40; ++i) holder.large["property" + i] = i;
holder.large.nested = { y: 5 };

const before = {
    small: addressOf(holder.small),
    large: addressOf(holder.large),
    array: addressOf(holder.array),
    hashed: addressOf(holder.hashed),
    hash: identityHashOf(holder.hashed),
    counter: addressOf(holder.counter),
    rope: addressOf(holder.rope),
};

const map = new Map([[holder.mapKey, "map value"]]);
const weakMap = new WeakMap([[holder.weakMapKey, "weak map value"]]);

runAfterMovingCollection(() => {
    test("objects with inline storage keep their properties when they move", () => {
        expect(addressOf(holder.small)).not.toBe(before.small);
        expect(holder.small.a).toBe(1);
        expect(holder.small.b).toBe("two");
        holder.small.c = 3;
        expect(Object.keys(holder.small)).toEqual(["a", "b", "c"]);
    });

    test("objects with out-of-line storage keep their properties when they move", () => {
        expect(addressOf(holder.large)).not.toBe(before.large);
        for (let i = 0; i < 40; ++i) expect(holder.large["property" + i]).toBe(i);
        expect(holder.large.nested.y).toBe(5);
    });

    test("arrays keep their elements when they move", () => {
        expect(addressOf(holder.array)).not.toBe(before.array);
        expect(holder.array.length).toBe(4);
        expect(holder.array[3].x).toBe(4);
        holder.array.push(5);
        expect(holder.array).toEqual([1, 2, 3, { x: 4 }, 5]);
    });

    test("identity hashes stay the same when cells move", () => {
        expect(addressOf(holder.hashed)).not.toBe(before.hashed);
        expect(identityHashOf(holder.hashed)).toBe(before.hash);
        const set = new Set([holder.hashed]);
        expect(set.has(holder.hashed)).toBeTrue();
    });

    test("closures keep their environments when they move", () => {
        expect(addressOf(holder.counter)).not.toBe(before.counter);
        expect(holder.counter()).toBe(2);
        expect(holder.counter()).toBe(3);
    });

    test("strings keep their contents when they move", () => {
        expect(addressOf(holder.rope)).not.toBe(before.rope);
        expect(holder.rope).toBe("x".repeat(30) + "y".repeat(30));
        expect(holder.substring).toBe("xxxxxyyyyy");
        expect(holder.flat).toBe("60!");
    });

    test("maps and weak maps find their keys after the collection", () => {
        expect(map.get(holder.mapKey)).toBe("map value");
        expect(weakMap.get(holder.weakMapKey)).toBe("weak map value");
        expect(map.has({})).toBeFalse();
    });
});

runAfterMovingCollection(() => {
    const addressAfterFirstMove = addressOf(holder.small);
    runAfterMovingCollection(() => {
        test("callbacks that a callback asks for run after another collection that moves cells", () => {
            expect(addressOf(holder.small)).not.toBe(addressAfterFirstMove);
            expect(holder.small.c).toBe(3);
        });
    });
});
