test("basic functionality", () => {
    expect(Map.prototype.delete).toHaveLength(1);

    const map = new Map([
        ["a", 0],
        ["b", 1],
        ["c", 2],
    ]);
    expect(map).toHaveSize(3);
    expect(map.delete("b")).toBeTrue();
    expect(map).toHaveSize(2);
    expect(map.delete("b")).toBeFalse();
    expect(map).toHaveSize(2);
});

describe("modification with active iterators", () => {
    test("deleted element is skipped", () => {
        const map = new Map([
            [1, 2],
            [3, 4],
            [5, 6],
        ]);
        const iterator = map.entries();

        expect(iterator.next()).toBeIteratorResultWithValue([1, 2]);

        expect(map.delete(3)).toBeTrue();

        expect(iterator.next()).toBeIteratorResultWithValue([5, 6]);

        expect(iterator.next()).toBeIteratorResultDone();
    });

    test("if rest of elements is deleted skip immediately to done", () => {
        const map = new Map([[-1, -1]]);

        for (let i = 1; i <= 25; ++i) map.set(i, i);

        const iterator = map.entries();

        expect(iterator.next()).toBeIteratorResultWithValue([-1, -1]);

        for (let i = 1; i <= 25; ++i) expect(map.delete(i)).toBeTrue();

        expect(iterator.next()).toBeIteratorResultDone();
        expect(iterator.next()).toBeIteratorResultDone();
    });

    test("deleting elements which were already visited has no effect", () => {
        const map = new Map([
            [1, 2],
            [3, 4],
            [5, 6],
        ]);

        const iterator = map.entries();

        expect(iterator.next()).toBeIteratorResultWithValue([1, 2]);

        expect(map.delete(1)).toBeTrue();

        expect(iterator.next()).toBeIteratorResultWithValue([3, 4]);

        expect(map.delete(3)).toBeTrue();

        expect(iterator.next()).toBeIteratorResultWithValue([5, 6]);

        expect(map.delete(5)).toBeTrue();
        expect(map.delete(7)).toBeFalse();

        expect(iterator.next()).toBeIteratorResultDone();
        expect(iterator.next()).toBeIteratorResultDone();
    });

    test("deleting the last element before the iterator visited it means you immediately get end", () => {
        const map = new Map([[1, 2]]);

        const iterator = map.entries();

        expect(map.delete(1)).toBeTrue();

        expect(iterator.next()).toBeIteratorResultDone();
        expect(iterator.next()).toBeIteratorResultDone();
    });
});
