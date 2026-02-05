describe("errors", () => {
    function TestError() {}

    test("called with non-Object", () => {
        expect(() => {
            Iterator.concat(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not an object");

        expect(() => {
            Iterator.concat([1], Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not an object");
    });

    test("iterator method throws an exception", () => {
        const iterable = {
            get [Symbol.iterator]() {
                throw new TestError();
            },
        };

        expect(() => {
            Iterator.concat(iterable);
        }).toThrow(TestError);
    });

    test("iterator method returns a non-function", () => {
        const iterable = {
            get [Symbol.iterator]() {
                return Symbol.hasInstance;
            },
        };

        expect(() => {
            Iterator.concat(iterable);
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not a function");
    });

    test("next method throws an exception", () => {
        const badIterator = {
            next() {
                throw new TestError();
            },
        };

        const iterable = {
            [Symbol.iterator]() {
                return badIterator;
            },
        };

        const iterator = Iterator.concat(iterable);

        expect(() => {
            iterator.next();
        }).toThrow(TestError);
    });

    test("next method returns a non-Object", () => {
        const badIterator = {
            next() {
                return null;
            },
        };

        const iterable = {
            [Symbol.iterator]() {
                return badIterator;
            },
        };

        const iterator = Iterator.concat(iterable);

        expect(() => {
            iterator.next();
        }).toThrowWithMessage(TypeError, "iterator.next() returned a non-object value");
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Iterator.concat).toHaveLength(0);
    });

    test("called with zero arguments", () => {
        const iterator = Iterator.concat();

        const result = iterator.next();
        expect(result.value).toBeUndefined();
        expect(result.done).toBeTrue();
    });

    test("called with one argument", () => {
        const array = [1, 2, 3];

        const iterator = Iterator.concat(array);

        for (let i = 0; i < array.length; ++i) {
            const result = iterator.next();
            expect(result.value).toBe(i + 1);
            expect(result.done).toBeFalse();
        }

        const result = iterator.next();
        expect(result.value).toBeUndefined();
        expect(result.done).toBeTrue();
    });

    test("called with several arguments", () => {
        const iterables = [[], [1], [2, 3], [4, 5, 6], [7, 8, 9, 10]];
        const length = iterables.reduce((sum, array) => sum + array.length, 0);

        const iterator = Iterator.concat(...iterables);

        for (let i = 0; i < length; ++i) {
            const result = iterator.next();
            expect(result.value).toBe(i + 1);
            expect(result.done).toBeFalse();
        }

        const result = iterator.next();
        expect(result.value).toBeUndefined();
        expect(result.done).toBeTrue();
    });
});
