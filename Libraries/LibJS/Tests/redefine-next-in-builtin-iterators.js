describe("redefine next() in built in iterators", () => {
    test("redefine next() in ArrayIteratorPrototype", () => {
        let arrayIteratorPrototype = Object.getPrototypeOf([].values());
        let originalNext = arrayIteratorPrototype.next;
        let counter = 0;
        arrayIteratorPrototype.next = function () {
            counter++;
            return originalNext.apply(this, arguments);
        };
        for (let i of [1, 2, 3]) {
        }
        expect(counter).toBe(4);
    });

    test("redefine next() in MapIteratorPrototype", () => {
        let m = new Map([
            [1, 1],
            [2, 2],
            [3, 3],
        ]);
        let mapIteratorPrototype = Object.getPrototypeOf(m.values());
        let originalNext = mapIteratorPrototype.next;
        let counter = 0;
        mapIteratorPrototype.next = function () {
            counter++;
            return originalNext.apply(this, arguments);
        };
        for (let v of m.values()) {
        }
        expect(counter).toBe(4);
    });

    test("redefine next() in SetIteratorPrototype", () => {
        let s = new Set([1, 2, 3]);
        let setIteratorPrototype = Object.getPrototypeOf(s.values());
        let originalNext = setIteratorPrototype.next;
        let counter = 0;
        setIteratorPrototype.next = function () {
            counter++;
            return originalNext.apply(this, arguments);
        };
        for (let v of s.values()) {
        }
        expect(counter).toBe(4);
    });
});
