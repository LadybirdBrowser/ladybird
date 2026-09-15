describe("from Set and Map", () => {
    test("Set", () => {
        const a = Array.from(new Set([1, "two", 3, "two"]));
        expect(a instanceof Array).toBeTrue();
        expect(a).toEqual([1, "two", 3]);
        expect(Array.from(new Set())).toEqual([]);
    });

    test("Map", () => {
        const a = Array.from(
            new Map([
                ["a", 1],
                ["b", 2],
            ])
        );
        expect(a).toEqual([
            ["a", 1],
            ["b", 2],
        ]);
        expect(a[0] instanceof Array).toBeTrue();
        expect(a[0]).not.toBe(a[1]);
        expect(Array.from(new Map())).toEqual([]);
    });

    test("Set iterators", () => {
        const set = new Set([1, 2, 3]);
        expect(Array.from(set.values())).toEqual([1, 2, 3]);
        expect(Array.from(set.keys())).toEqual([1, 2, 3]);
        expect(Array.from(set.entries())).toEqual([
            [1, 1],
            [2, 2],
            [3, 3],
        ]);
    });

    test("Map iterators", () => {
        const map = new Map([
            ["a", 1],
            ["b", 2],
        ]);
        expect(Array.from(map.keys())).toEqual(["a", "b"]);
        expect(Array.from(map.values())).toEqual([1, 2]);
        expect(Array.from(map.entries())).toEqual([
            ["a", 1],
            ["b", 2],
        ]);
    });

    test("with mapFn and thisArg", () => {
        const receiver = { offset: 100 };
        const seen = [];
        const a = Array.from(
            new Set([5, 6, 7]),
            function (value, index) {
                seen.push([this, value, index, arguments.length]);
                return value + index + this.offset;
            },
            receiver
        );
        expect(a).toEqual([105, 107, 109]);
        expect(seen).toEqual([
            [receiver, 5, 0, 2],
            [receiver, 6, 1, 2],
            [receiver, 7, 2, 2],
        ]);

        const b = Array.from(new Map([["k", "v"]]), (entry, index) => entry.join("=") + index);
        expect(b).toEqual(["k=v0"]);

        const c = Array.from(new Map([["k", "v"]]).values(), (value, index) => value + index);
        expect(c).toEqual(["v0"]);
    });

    test("partially consumed iterator", () => {
        const set = new Set([1, 2, 3, 4]);
        const setIterator = set.values();
        expect(setIterator.next()).toEqual({ value: 1, done: false });
        expect(Array.from(setIterator)).toEqual([2, 3, 4]);
        expect(setIterator.next()).toEqual({ value: undefined, done: true });
        set.add(5);
        expect(setIterator.next()).toEqual({ value: undefined, done: true });

        const map = new Map([
            [1, "a"],
            [2, "b"],
            [3, "c"],
        ]);
        const mapIterator = map.entries();
        mapIterator.next();
        expect(Array.from(mapIterator, entry => entry[1])).toEqual(["b", "c"]);
        expect(mapIterator.next()).toEqual({ value: undefined, done: true });
    });

    test("mapFn that consumes the iterator being walked", () => {
        const iterator = new Set([1, 2, 3, 4]).values();
        const a = Array.from(iterator, value => {
            if (value === 1) iterator.next();
            return value;
        });
        expect(a).toEqual([1, 3, 4]);
    });

    test("mapFn that adds entries", () => {
        const set = new Set([1, 2]);
        const a = Array.from(set, value => {
            if (value < 4) set.add(value + 2);
            return value;
        });
        expect(a).toEqual([1, 2, 3, 4, 5]);

        const map = new Map([[1, 1]]);
        const b = Array.from(map, ([key]) => {
            if (key < 3) map.set(key + 1, key + 1);
            return key;
        });
        expect(b).toEqual([1, 2, 3]);
    });

    test("mapFn that deletes entries", () => {
        const set = new Set([1, 2, 3, 4, 5]);
        const a = Array.from(set, value => {
            if (value === 1) {
                set.delete(2);
                set.delete(4);
            }
            return value;
        });
        expect(a).toEqual([1, 3, 5]);

        const map = new Map([
            ["a", 1],
            ["b", 2],
            ["c", 3],
        ]);
        const b = Array.from(map.keys(), key => {
            if (key === "a") map.delete("b");
            return key;
        });
        expect(b).toEqual(["a", "c"]);
    });

    test("mapFn that deletes and re-adds the current entry", () => {
        const set = new Set([1, 2, 3]);
        let readded = false;
        const a = Array.from(set, value => {
            if (value === 1 && !readded) {
                readded = true;
                set.delete(1);
                set.add(1);
            }
            return value;
        });
        expect(a).toEqual([1, 2, 3, 1]);
    });

    test("mapFn that deletes many entries, compacting the storage", () => {
        const set = new Set();
        for (let i = 0; i < 100; ++i) set.add(i);
        const a = Array.from(set, value => {
            if (value === 10) {
                for (let i = 0; i < 95; ++i) {
                    if (i !== 50) set.delete(i);
                }
                set.add(100);
            }
            return value;
        });
        expect(a).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 50, 95, 96, 97, 98, 99, 100]);
    });

    test("mapFn that clears the collection", () => {
        const set = new Set([1, 2, 3]);
        expect(
            Array.from(set, value => {
                set.clear();
                return value;
            })
        ).toEqual([1]);

        const map = new Map([
            [1, 1],
            [2, 2],
        ]);
        expect(
            Array.from(map.values(), value => {
                map.clear();
                return value;
            })
        ).toEqual([1]);
    });

    test("mapFn that clears the collection and adds entries", () => {
        const set = new Set([1, 2, 3]);
        const a = Array.from(set, value => {
            if (value === 1) {
                set.clear();
                set.add(4);
            }
            return value;
        });
        expect(a).toEqual([1, 4]);
    });

    test("mapFn that throws", () => {
        const set = new Set([1, 2, 3]);
        let calls = 0;
        expect(() => {
            Array.from(set, () => {
                ++calls;
                throw new Error("stop");
            });
        }).toThrowWithMessage(Error, "stop");
        expect(calls).toBe(1);
    });

    test("subclass constructor", () => {
        class MyArray extends Array {}
        const a = MyArray.from(new Set([1, 2]));
        expect(a instanceof MyArray).toBeTrue();
        expect(a).toHaveLength(2);
        expect(a[0]).toBe(1);
        expect(a[1]).toBe(2);

        const b = MyArray.from(new Map([["k", "v"]]).entries());
        expect(b instanceof MyArray).toBeTrue();
        expect(b[0]).toEqual(["k", "v"]);
    });

    test("non-constructor this", () => {
        const a = Array.from.call({}, new Set([1, 2]));
        expect(Array.isArray(a)).toBeTrue();
        expect(a).toEqual([1, 2]);
    });

    test("observes a patched Set.prototype[Symbol.iterator]", () => {
        const original = Set.prototype[Symbol.iterator];
        Set.prototype[Symbol.iterator] = function* () {
            yield "patched";
        };
        try {
            expect(Array.from(new Set([1, 2]))).toEqual(["patched"]);
        } finally {
            Set.prototype[Symbol.iterator] = original;
        }
        expect(Array.from(new Set([1, 2]))).toEqual([1, 2]);
    });

    test("observes a Symbol.iterator getter on a Set", () => {
        const set = new Set([1, 2]);
        let getterCalls = 0;
        Object.defineProperty(set, Symbol.iterator, {
            get() {
                ++getterCalls;
                set.add(3);
                return Set.prototype.values;
            },
        });
        expect(Array.from(set)).toEqual([1, 2, 3]);
        expect(getterCalls).toBe(1);
    });

    test("Set.prototype.values as Symbol.iterator of a non-Set throws", () => {
        expect(() => {
            Array.from.call(Array, { __proto__: Map.prototype, [Symbol.iterator]: Set.prototype.values });
        }).toThrow(TypeError);
    });

    test("observes a patched %SetIteratorPrototype%.next", () => {
        const setIteratorPrototype = Object.getPrototypeOf(new Set().values());
        const originalNext = setIteratorPrototype.next;
        let calls = 0;
        setIteratorPrototype.next = function () {
            ++calls;
            return originalNext.call(this);
        };
        try {
            const set = new Set([1, 2]);
            expect(Array.from(set)).toEqual([1, 2]);
            expect(calls).toBe(3);
            calls = 0;
            expect(Array.from(set.values())).toEqual([1, 2]);
            expect(calls).toBe(3);
            calls = 0;
            expect(Array.from(set, value => value * 2)).toEqual([2, 4]);
            expect(calls).toBe(3);
        } finally {
            setIteratorPrototype.next = originalNext;
        }
    });

    test("observes a %SetIteratorPrototype%.next getter", () => {
        const setIteratorPrototype = Object.getPrototypeOf(new Set().values());
        const originalDescriptor = Object.getOwnPropertyDescriptor(setIteratorPrototype, "next");
        let getterCalls = 0;
        Object.defineProperty(setIteratorPrototype, "next", {
            configurable: true,
            get() {
                ++getterCalls;
                return originalDescriptor.value;
            },
        });
        try {
            expect(Array.from(new Set([1, 2]))).toEqual([1, 2]);
            expect(getterCalls).toBe(1);
        } finally {
            Object.defineProperty(setIteratorPrototype, "next", originalDescriptor);
        }
    });

    test("observes a patched Map.prototype[Symbol.iterator]", () => {
        const original = Map.prototype[Symbol.iterator];
        Map.prototype[Symbol.iterator] = Map.prototype.keys;
        try {
            expect(Array.from(new Map([["k", "v"]]))).toEqual(["k"]);
        } finally {
            Map.prototype[Symbol.iterator] = original;
        }
        expect(Array.from(new Map([["k", "v"]]))).toEqual([["k", "v"]]);
    });

    test("observes a patched %MapIteratorPrototype%.next", () => {
        const mapIteratorPrototype = Object.getPrototypeOf(new Map().entries());
        const originalNext = mapIteratorPrototype.next;
        let calls = 0;
        mapIteratorPrototype.next = function () {
            ++calls;
            const result = originalNext.call(this);
            if (!result.done) result.value = "patched";
            return result;
        };
        try {
            const map = new Map([
                ["a", 1],
                ["b", 2],
            ]);
            expect(Array.from(map)).toEqual(["patched", "patched"]);
            expect(calls).toBe(3);
            calls = 0;
            expect(Array.from(map.values())).toEqual(["patched", "patched"]);
            expect(calls).toBe(3);
        } finally {
            mapIteratorPrototype.next = originalNext;
        }
        expect(Array.from(new Map([["k", "v"]]))).toEqual([["k", "v"]]);
    });

    test("observes an own next property on a Set iterator", () => {
        const iterator = new Set([1, 2]).values();
        let calls = 0;
        iterator.next = function () {
            return ++calls < 3 ? { value: calls * 10, done: false } : { done: true };
        };
        expect(Array.from(iterator)).toEqual([10, 20]);
    });

    test("observes a patched %IteratorPrototype%[Symbol.iterator] on a Map iterator", () => {
        const iteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf(new Map().values()));
        const original = iteratorPrototype[Symbol.iterator];
        iteratorPrototype[Symbol.iterator] = function () {
            return [7, 8][Symbol.iterator]();
        };
        try {
            expect(Array.from(new Map([["k", "v"]]).values())).toEqual([7, 8]);
        } finally {
            iteratorPrototype[Symbol.iterator] = original;
        }
    });

    test("defines own properties instead of calling Array.prototype setters", () => {
        let setterCalls = 0;
        Object.defineProperty(Array.prototype, "0", {
            configurable: true,
            set() {
                ++setterCalls;
            },
        });
        let fromSet, fromMap, fromIterator;
        try {
            fromSet = Array.from(new Set(["x"]));
            fromMap = Array.from(new Map([["k", "v"]]));
            fromIterator = Array.from(new Set(["y"]).values(), value => value);
        } finally {
            delete Array.prototype[0];
        }
        expect(setterCalls).toBe(0);
        expect(Object.getOwnPropertyDescriptor(fromSet, 0).value).toBe("x");
        expect(Object.getOwnPropertyDescriptor(fromMap, 0).value).toEqual(["k", "v"]);
        expect(Object.getOwnPropertyDescriptor(fromIterator, 0).value).toBe("y");
    });
});

for (const Collection of [Set, Map]) {
    for (const installDuringMapping of [false, true]) {
        test(`${Collection.name} closes iterator when mapping throws, late return: ${installDuringMapping}`, () => {
            const collection = new Collection([
                [1, 2],
                [3, 4],
            ]);
            const prototype = Object.getPrototypeOf(collection[Symbol.iterator]());
            const error = new Error("mapping failed");
            let closedIterator;
            let calls = 0;
            function close() {
                ++calls;
                closedIterator = this;
                throw new Error("closing failed");
            }
            try {
                if (!installDuringMapping) prototype.return = close;
                let caught;
                try {
                    Array.from(collection, () => {
                        if (installDuringMapping) prototype.return = close;
                        throw error;
                    });
                } catch (exception) {
                    caught = exception;
                }
                expect(caught).toBe(error);
                expect(calls).toBe(1);
                expect(closedIterator.next()).toEqual({
                    value: [3, 4],
                    done: false,
                });
            } finally {
                delete prototype.return;
            }
        });
    }
}
