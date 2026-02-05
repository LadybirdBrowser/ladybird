test("basic functionality", () => {
    expect(Map.prototype.set).toHaveLength(2);

    const map = new Map([
        ["a", 0],
        ["b", 1],
        ["c", 2],
    ]);
    expect(map).toHaveSize(3);
    expect(map.set("d", 3)).toBe(map);
    expect(map).toHaveSize(4);
    expect(map.set("a", -1)).toBe(map);
    expect(map).toHaveSize(4);
});

describe("modification with active iterators", () => {
    test("added element is visited (after initial elements)", () => {
        const map = new Map([
            [1, 2],
            [5, 6],
        ]);
        const iterator = map.entries();

        expect(iterator.next()).toBeIteratorResultWithValue([1, 2]);

        map.set(3, 4);

        expect(iterator.next()).toBeIteratorResultWithValue([5, 6]);

        expect(iterator.next()).toBeIteratorResultWithValue([3, 4]);

        expect(iterator.next()).toBeIteratorResultDone();
        expect(iterator.next()).toBeIteratorResultDone();
    });

    test("entries added after iterator is done are not visited", () => {
        const map = new Map([[1, 2]]);

        const iterator = map.entries();

        expect(iterator.next()).toBeIteratorResultWithValue([1, 2]);

        expect(iterator.next()).toBeIteratorResultDone();

        map.set(3, 4);

        expect(iterator.next()).toBeIteratorResultDone();
    });

    test("entries which are deleted and then added are visited at the end", () => {
        const map = new Map([
            [1, 2],
            [3, 4],
        ]);

        const iterator = map.entries();

        expect(iterator.next()).toBeIteratorResultWithValue([1, 2]);

        expect(map.delete(1)).toBeTrue();
        map.set(1, 10);

        expect(map.delete(3)).toBeTrue();
        map.set(3, 11);

        expect(iterator.next()).toBeIteratorResultWithValue([1, 10]);

        expect(iterator.next()).toBeIteratorResultWithValue([3, 11]);

        expect(iterator.next()).toBeIteratorResultDone();
        expect(iterator.next()).toBeIteratorResultDone();
    });

    test("entries which added to empty map after iterator created are still visited", () => {
        const map = new Map();

        const iteratorImmediateDone = map.entries();
        expect(iteratorImmediateDone.next()).toBeIteratorResultDone();

        const iterator = map.entries();

        map.set(1, 2);

        expect(iterator.next()).toBeIteratorResultWithValue([1, 2]);

        expect(map.delete(1)).toBeTrue();

        map.set(3, 4);

        expect(iterator.next()).toBeIteratorResultWithValue([3, 4]);

        expect(iterator.next()).toBeIteratorResultDone();
        expect(iterator.next()).toBeIteratorResultDone();
    });
});
