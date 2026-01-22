test("basic functionality", () => {
    expect(Set.prototype.add).toHaveLength(1);

    const set = new Set(["a", "b", "c"]);
    expect(set).toHaveSize(3);
    expect(set.add("d")).toBe(set);
    expect(set).toHaveSize(4);
    expect(set.add("a")).toBe(set);
    expect(set).toHaveSize(4);
});

describe("elements added after iteration start are still visited", () => {
    test("element added after iterator", () => {
        const set = new Set();
        const iterator = set.values();
        set.add(1);
        expect(iterator.next()).toBeIteratorResultWithValue(1);
        expect(iterator.next()).toBeIteratorResultDone();
        expect(iterator.next()).toBeIteratorResultDone();
    });

    test("elements (re)added after deleting", () => {
        const set = new Set();
        const iterator1 = set.values();
        set.add(1);
        set.add(2);
        set.clear();
        const iterator2 = set.values();
        set.add(1);
        expect(iterator1.next()).toBeIteratorResultWithValue(1);
        expect(iterator1.next()).toBeIteratorResultDone();
        expect(iterator1.next()).toBeIteratorResultDone();

        expect(iterator2.next()).toBeIteratorResultWithValue(1);
        expect(iterator2.next()).toBeIteratorResultDone();
        expect(iterator2.next()).toBeIteratorResultDone();
    });
});
