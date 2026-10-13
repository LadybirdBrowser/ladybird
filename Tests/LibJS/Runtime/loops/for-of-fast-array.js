// for-of loops and array destructuring patterns step through plain arrays without making array iterators. These tests
// check that they still behave like array iterators do.

const ArrayIteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());

function collect(iterable) {
    const values = [];
    for (const value of iterable) values.push(value);
    return values;
}

function firstTwo(iterable) {
    const [first, second] = iterable;
    return [first, second];
}

function restAfterFirst(iterable) {
    const [first, ...rest] = iterable;
    return [first, rest];
}

describe("elements", () => {
    test("packed arrays", () => {
        expect(collect([1, 2, 3])).toEqual([1, 2, 3]);
        expect(collect([])).toEqual([]);
        expect(firstTwo([1, 2, 3])).toEqual([1, 2]);
        expect(firstTwo([1])).toEqual([1, undefined]);
        expect(restAfterFirst([1, 2, 3])).toEqual([1, [2, 3]]);
        expect(restAfterFirst([])).toEqual([undefined, []]);
    });

    test("holes read undefined", () => {
        expect(collect([1, , 3])).toEqual([1, undefined, 3]);
        expect(collect(new Array(3))).toEqual([undefined, undefined, undefined]);
        expect(firstTwo(new Array(5))).toEqual([undefined, undefined]);
        expect(restAfterFirst([1, , 3])).toEqual([1, [undefined, 3]]);
    });

    test("holes read the prototype chain", () => {
        Array.prototype[1] = "from Array.prototype";
        Object.prototype[2] = "from Object.prototype";
        try {
            expect(collect([0, , , 3])).toEqual([0, "from Array.prototype", "from Object.prototype", 3]);
            expect(firstTwo(new Array(2))).toEqual([undefined, "from Array.prototype"]);
            expect(restAfterFirst(new Array(3))).toEqual([
                undefined,
                ["from Array.prototype", "from Object.prototype"],
            ]);
        } finally {
            delete Array.prototype[1];
            delete Object.prototype[2];
        }
    });

    test("getters on the prototype run once per hole", () => {
        let getterCalls = 0;
        Object.defineProperty(Array.prototype, 1, {
            get() {
                ++getterCalls;
                return "from getter";
            },
            configurable: true,
        });
        try {
            let seen = "";
            for (const value of new Array(3)) seen += `${value},`;
            expect(seen).toBe("undefined,from getter,undefined,");
            expect(getterCalls).toBe(1);
        } finally {
            delete Array.prototype[1];
        }
    });

    test("subclasses of Array", () => {
        class MyArray extends Array {}
        const array = MyArray.from([1, 2, 3]);
        expect(collect(array)).toEqual([1, 2, 3]);
        expect(firstTwo(array)).toEqual([1, 2]);
    });

    test("frozen arrays", () => {
        const array = Object.freeze([1, 2, 3]);
        expect(collect(array)).toEqual([1, 2, 3]);
        expect(restAfterFirst(array)).toEqual([1, [2, 3]]);
    });
});

describe("changes during the iteration", () => {
    test("arrays that grow", () => {
        const array = [1, 2];
        const values = [];
        for (const value of array) {
            values.push(value);
            if (array.length < 4) array.push(value * 10);
        }
        expect(values).toEqual([1, 2, 10, 20]);
    });

    test("arrays that shrink", () => {
        const array = [1, 2, 3, 4];
        const values = [];
        for (const value of array) {
            values.push(value);
            array.length = 2;
        }
        expect(values).toEqual([1, 2]);
    });

    test("arrays that get holes", () => {
        const array = [1, 2, 3];
        const values = [];
        for (const value of array) {
            values.push(value);
            delete array[1];
        }
        expect(values).toEqual([1, undefined, 3]);
    });

    test("an iteration that is done stays done when the array grows", () => {
        const array = [1];
        const values = [];
        for (const value of array) values.push(value);
        array.push(2);
        expect(values).toEqual([1]);

        const [first, second, third] = array;
        expect([first, second, third]).toEqual([1, 2, undefined]);
    });

    test("next replaced during the iteration is not called", () => {
        const originalNext = ArrayIteratorPrototype.next;
        const values = [];
        try {
            for (const value of [1, 2, 3]) {
                values.push(value);
                ArrayIteratorPrototype.next = () => ({ value: "replaced", done: false });
            }
        } finally {
            ArrayIteratorPrototype.next = originalNext;
        }
        expect(values).toEqual([1, 2, 3]);
    });
});

describe("iterator methods", () => {
    test("an own iterator method of the array", () => {
        const array = [1, 2, 3];
        array[Symbol.iterator] = function* () {
            yield "own";
        };
        expect(collect(array)).toEqual(["own"]);
        expect(firstTwo(array)).toEqual(["own", undefined]);
    });

    test("an iterator getter runs once per iteration", () => {
        let getterCalls = 0;
        const array = [1, 2, 3];
        Object.defineProperty(array, Symbol.iterator, {
            get() {
                ++getterCalls;
                return Array.prototype.values;
            },
        });
        expect(collect(array)).toEqual([1, 2, 3]);
        expect(firstTwo(array)).toEqual([1, 2]);
        expect(getterCalls).toBe(2);
    });

    test("an iterator that is an array with a next property that is not callable", () => {
        const iterator = [1, 2, 3];
        iterator.next = 0;
        const iterable = { [Symbol.iterator]: () => iterator };
        expect(() => collect(iterable)).toThrow(TypeError);
        expect(() => firstTwo(iterable)).toThrow(TypeError);
    });

    test("arrays of another iteration method", () => {
        const array = [1, 2, 3];
        expect(collect(array.keys())).toEqual([0, 1, 2]);
        expect(collect(array.entries())).toEqual([
            [0, 1],
            [1, 2],
            [2, 3],
        ]);
        expect(firstTwo(array.values())).toEqual([1, 2]);
    });
});

describe("closing", () => {
    function expectClosedAt(close, expectedNextValue) {
        const receivers = [];
        ArrayIteratorPrototype.return = function () {
            receivers.push(this);
            expect(Object.getPrototypeOf(this)).toBe(ArrayIteratorPrototype);
            expect(this.next()).toEqual(expectedNextValue);
            return {};
        };
        try {
            close();
        } finally {
            delete ArrayIteratorPrototype.return;
        }
        expect(receivers).toHaveLength(1);
    }

    test("break, return and throw close at the next element", () => {
        expectClosedAt(
            () => {
                for (const value of [1, 2, 3]) break;
            },
            { value: 2, done: false }
        );
        expectClosedAt(
            () => {
                (() => {
                    for (const value of [1, 2, 3]) if (value === 2) return;
                })();
            },
            { value: 3, done: false }
        );
        expectClosedAt(
            () => {
                expect(() => {
                    for (const value of [1, 2, 3]) throw new Error("thrown");
                }).toThrowWithMessage(Error, "thrown");
            },
            { value: 2, done: false }
        );
    });

    test("a return method added during the iteration is called", () => {
        const receivers = [];
        try {
            for (const value of [1, 2, 3]) {
                ArrayIteratorPrototype.return = function () {
                    receivers.push(this.next());
                    return {};
                };
                break;
            }
        } finally {
            delete ArrayIteratorPrototype.return;
        }
        expect(receivers).toEqual([{ value: 2, done: false }]);
    });

    test("destructuring closes unless the pattern reads past the end", () => {
        expectClosedAt(
            () => {
                const [first] = [1, 2, 3];
            },
            { value: 2, done: false }
        );
        expectClosedAt(
            () => {
                const [first, second] = [1, 2];
            },
            { value: undefined, done: true }
        );

        let returnCalls = 0;
        ArrayIteratorPrototype.return = () => {
            ++returnCalls;
            return {};
        };
        try {
            const [first, second] = [1];
            const [third, ...rest] = [1, 2, 3];
        } finally {
            delete ArrayIteratorPrototype.return;
        }
        expect(returnCalls).toBe(0);
    });

    test("a throwing return method and a throwing body", () => {
        ArrayIteratorPrototype.return = () => {
            throw new Error("from return");
        };
        try {
            expect(() => {
                for (const value of [1, 2]) throw new Error("from body");
            }).toThrowWithMessage(Error, "from body");
            expect(() => {
                for (const value of [1, 2]) break;
            }).toThrowWithMessage(Error, "from return");
        } finally {
            delete ArrayIteratorPrototype.return;
        }
    });
});

describe("other consumers of iterators", () => {
    test("yield* over arrays", () => {
        function* delegate(array) {
            return yield* array;
        }
        expect([...delegate([1, 2, 3])]).toEqual([1, 2, 3]);
        const generator = delegate([1, 2, 3]);
        generator.next();
        expect(generator.return(5)).toEqual({ value: 5, done: true });
    });

    test("a generator suspended inside a for-of over an array", () => {
        function* generator(array) {
            for (const value of array) yield value * 2;
        }
        const iterator = generator([1, 2, 3]);
        expect(iterator.next()).toEqual({ value: 2, done: false });
        expect(iterator.next()).toEqual({ value: 4, done: false });
        expect(iterator.return(7)).toEqual({ value: 7, done: true });
        expect([...generator([4, 5])]).toEqual([8, 10]);
    });

    test("for await over arrays", async () => {
        const values = [];
        for await (const value of [1, Promise.resolve(2), 3]) values.push(value);
        expect(values).toEqual([1, 2, 3]);
    });

    test("nested patterns, defaults and elisions", () => {
        const [[a, b] = [9, 9], , c = 7, ...[d, e]] = [[1, 2], 0, undefined, 4, 5];
        expect([a, b, c, d, e]).toEqual([1, 2, 7, 4, 5]);
        const [x = 1, y = 2] = [];
        expect([x, y]).toEqual([1, 2]);
    });

    test("assignment patterns", () => {
        let first;
        let second;
        [first, second] = [1, 2, 3];
        expect([first, second]).toEqual([1, 2]);
        [second, first] = [first, second];
        expect([first, second]).toEqual([2, 1]);
    });
});
