describe("redefine next() in built in iterators", () => {
    test("override next() on individual Array iterator via setPrototypeOf", () => {
        const iter = [1, 2, 3][Symbol.iterator]();
        const originalNext = iter.next;
        let count = 0;
        const newProto = {
            next(...args) {
                count++;
                return originalNext.apply(this, args);
            },
            [Symbol.iterator]() {
                return this;
            },
        };
        Object.setPrototypeOf(iter, newProto);
        for (const v of iter) {
        }
        expect(count).toBe(4);
    });

    test("override next() on individual Map iterator via setPrototypeOf", () => {
        const map = new Map([
            [1, 1],
            [2, 2],
            [3, 3],
        ]);
        const iter = map.values();
        const originalNext = iter.next;
        let count = 0;
        const newProto = {
            next(...args) {
                count++;
                return originalNext.apply(this, args);
            },
            [Symbol.iterator]() {
                return this;
            },
        };
        Object.setPrototypeOf(iter, newProto);
        for (const v of iter) {
        }
        expect(count).toBe(4);
    });

    test("override next() on individual Set iterator via setPrototypeOf", () => {
        const set = new Set([1, 2, 3]);
        const iter = set.values();
        const originalNext = iter.next;
        let count = 0;
        const newProto = {
            next(...args) {
                count++;
                return originalNext.apply(this, args);
            },
            [Symbol.iterator]() {
                return this;
            },
        };
        Object.setPrototypeOf(iter, newProto);
        for (const v of iter) {
        }
        expect(count).toBe(4);
    });
});
