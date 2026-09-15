describe("Set and Map iteration while the collection changes", () => {
    test("deleting the next entry of a Set iterator skips it", () => {
        const set = new Set([1, 2, 3]);
        const iterator = set.values();
        expect(iterator.next()).toEqual({ value: 1, done: false });
        set.delete(2);
        expect(iterator.next()).toEqual({ value: 3, done: false });
        expect(iterator.next()).toEqual({ value: undefined, done: true });
    });

    test("deleting the last visited entry of a Map iterator does not skip the next one", () => {
        const map = new Map([
            ["a", 1],
            ["b", 2],
            ["c", 3],
        ]);
        const iterator = map.keys();
        expect(iterator.next()).toEqual({ value: "a", done: false });
        map.delete("a");
        expect(iterator.next()).toEqual({ value: "b", done: false });
        expect(iterator.next()).toEqual({ value: "c", done: false });
        expect(iterator.next()).toEqual({ value: undefined, done: true });
    });

    test("entries added during iteration are visited", () => {
        const set = new Set([1]);
        const iterator = set.values();
        expect(iterator.next()).toEqual({ value: 1, done: false });
        set.add(2);
        expect(iterator.next()).toEqual({ value: 2, done: false });
        expect(iterator.next()).toEqual({ value: undefined, done: true });
        set.add(3);
        expect(iterator.next()).toEqual({ value: undefined, done: true });
    });

    test("an entry deleted and re-added during iteration is visited again", () => {
        const set = new Set([1, 2]);
        const iterator = set.values();
        expect(iterator.next()).toEqual({ value: 1, done: false });
        set.delete(1);
        set.add(1);
        expect(iterator.next()).toEqual({ value: 2, done: false });
        expect(iterator.next()).toEqual({ value: 1, done: false });
        expect(iterator.next()).toEqual({ value: undefined, done: true });
    });

    test("clear() ends iteration, but later additions are still visited", () => {
        const set = new Set([1, 2, 3]);
        const iterator = set.values();
        expect(iterator.next()).toEqual({ value: 1, done: false });
        set.clear();
        set.add(4);
        expect(iterator.next()).toEqual({ value: 4, done: false });
        expect(iterator.next()).toEqual({ value: undefined, done: true });

        const map = new Map([[1, 1]]);
        const mapIterator = map.entries();
        map.clear();
        expect(mapIterator.next()).toEqual({ value: undefined, done: true });
    });

    test("an iterator created before any entries sees them", () => {
        const map = new Map();
        const iterator = map.values();
        map.set("a", 1);
        expect(iterator.next()).toEqual({ value: 1, done: false });
    });

    test("iterators stay correct when many deletions compact the storage", () => {
        const set = new Set();
        for (let i = 0; i < 1000; ++i) set.add(i);
        const iterator = set.values();
        const seen = [];
        for (let i = 0; i < 10; ++i) seen.push(iterator.next().value);

        for (let i = 0; i < 1000; ++i) {
            if (i % 100 !== 0 && i !== 9) set.delete(i);
        }
        set.add("late");

        for (let result = iterator.next(); !result.done; result = iterator.next()) seen.push(result.value);
        expect(seen).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 100, 200, 300, 400, 500, 600, 700, 800, 900, "late"]);
        expect(Array.from(set)).toEqual([0, 9, 100, 200, 300, 400, 500, 600, 700, 800, 900, "late"]);
        expect(set.size).toBe(12);
    });

    test("an iterator positioned at a deleted entry survives compaction", () => {
        const map = new Map();
        for (let i = 0; i < 100; ++i) map.set(i, i * 2);
        const iterator = map.entries();
        expect(iterator.next().value).toEqual([0, 0]);
        for (let i = 0; i < 60; ++i) map.delete(i);
        expect(iterator.next().value).toEqual([60, 120]);
        expect(map.get(99)).toBe(198);
        expect(map.has(59)).toBeFalse();
    });

    test("forEach deleting the current entry does not skip the next one", () => {
        const set = new Set();
        for (let i = 0; i < 50; ++i) set.add(i);
        const seen = [];
        set.forEach(value => {
            seen.push(value);
            set.delete(value);
        });
        expect(seen).toHaveLength(50);
        expect(seen[49]).toBe(49);
        expect(set.size).toBe(0);
    });

    test("Map.prototype.forEach with deletions, re-additions and compaction", () => {
        const map = new Map();
        for (let i = 0; i < 40; ++i) map.set(i, `v${i}`);
        const seen = [];
        map.forEach((value, key) => {
            seen.push(key);
            if (key === 5 && value !== "again") {
                for (let i = 0; i < 40; ++i) {
                    if (i !== 6 && i !== 39) map.delete(i);
                }
                map.set(5, "again");
            }
        });
        expect(seen).toEqual([0, 1, 2, 3, 4, 5, 6, 39, 5]);
        expect(Array.from(map)).toEqual([
            [6, "v6"],
            [39, "v39"],
            [5, "again"],
        ]);
    });

    test("collection operations after compaction", () => {
        const map = new Map();
        for (let i = 0; i < 100; ++i) map.set(`k${i}`, i);
        for (let i = 0; i < 90; ++i) map.delete(`k${i}`);
        for (let i = 90; i < 100; ++i) expect(map.get(`k${i}`)).toBe(i);
        map.set("k95", "updated");
        map.set("new", 1);
        expect(Array.from(map.keys())).toEqual([
            "k90",
            "k91",
            "k92",
            "k93",
            "k94",
            "k95",
            "k96",
            "k97",
            "k98",
            "k99",
            "new",
        ]);
        expect(map.get("k95")).toBe("updated");
        expect(map.size).toBe(11);
    });

    test("spread observes live semantics of partially consumed iterators", () => {
        const set = new Set([1, 2, 3]);
        const iterator = set.values();
        iterator.next();
        set.delete(2);
        expect([...iterator]).toEqual([3]);
        expect(iterator.next()).toEqual({ value: undefined, done: true });
    });
});
