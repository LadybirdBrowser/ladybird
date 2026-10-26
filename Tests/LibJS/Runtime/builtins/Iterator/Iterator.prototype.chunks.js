describe("normal behavior", () => {
    test("function properties", () => {
        expect(Iterator.prototype.chunks).toHaveLength(1);
        expect(Iterator.prototype.chunks.name).toBe("chunks");
        expect(Object.getOwnPropertyDescriptor(Iterator.prototype, "chunks")).toEqual({
            value: Iterator.prototype.chunks,
            writable: true,
            enumerable: false,
            configurable: true,
        });
    });

    test("chunks of different sizes", () => {
        for (const [values, size, expected] of [
            [[], 2, []],
            [[1, 2, 3], 1, [[1], [2], [3]]],
            [
                [1, 2, 3, 4],
                2,
                [
                    [1, 2],
                    [3, 4],
                ],
            ],
            [[1, 2, 3, 4, 5], 2, [[1, 2], [3, 4], [5]]],
            [[1, 2], 3, [[1, 2]]],
            [[1, 2], 2 ** 32 - 1, [[1, 2]]],
            [[undefined, null], 2, [[undefined, null]]],
        ]) {
            expect(values.values().chunks(size).toArray()).toEqual(expected);
        }
    });

    test("iteration is lazy and reads next only once", () => {
        let nextLookups = 0;
        let nextCalls = 0;
        const source = {
            get next() {
                ++nextLookups;
                return function () {
                    expect(this).toBe(source);
                    expect(arguments).toHaveLength(0);
                    return { value: ++nextCalls, done: false };
                };
            },
            [Symbol.iterator]() {
                expect().fail("Symbol.iterator must not be called");
            },
        };
        const iterator = Iterator.prototype.chunks.call(source, 2);
        expect(nextLookups).toBe(1);
        expect(nextCalls).toBe(0);
        expect(iterator.next()).toEqual({ value: [1, 2], done: false });
        expect(nextCalls).toBe(2);
        expect(iterator.next()).toEqual({ value: [3, 4], done: false });
        expect(nextLookups).toBe(1);
    });

    test("yielded arrays are independent", () => {
        const value = {};
        const iterator = [value, 2, 3].values().chunks(1);
        const first = iterator.next().value;
        first.push(42);
        expect(iterator.next().value).toEqual([2]);
        expect(iterator.next().value).toEqual([3]);
        expect(first).toEqual([value, 42]);
    });

    test("return closes the underlying iterator before and after starting", () => {
        for (const started of [false, true]) {
            let returnCalls = 0;
            const source = {
                next() {
                    return { value: 1, done: false };
                },
                return() {
                    expect(this).toBe(source);
                    expect(arguments).toHaveLength(0);
                    ++returnCalls;
                    return {};
                },
            };
            const iterator = Iterator.prototype.chunks.call(source, 2);
            if (started) iterator.next();
            expect(iterator.return()).toEqual({ value: undefined, done: true });
            expect(iterator.next()).toEqual({ value: undefined, done: true });
            iterator.return();
            expect(returnCalls).toBe(1);
        }
    });

    test("the final partial chunk does not advance or close an exhausted iterator", () => {
        for (const close of [false, true]) {
            let nextCalls = 0;
            const source = {
                next() {
                    ++nextCalls;
                    return { value: 42, done: nextCalls > 1 };
                },
                return() {
                    expect().fail("An exhausted iterator must not be closed");
                },
            };
            const iterator = Iterator.prototype.chunks.call(source, 2);
            expect(iterator.next()).toEqual({ value: [42], done: false });
            expect(close ? iterator.return() : iterator.next()).toEqual({ value: undefined, done: true });
            expect(iterator.next()).toEqual({ value: undefined, done: true });
            expect(nextCalls).toBe(2);
        }
    });
});

describe("errors", () => {
    test("receiver must be an object", () => {
        for (const receiver of [undefined, null, false, 1, "abc", Symbol(), 1n]) {
            expect(() => Iterator.prototype.chunks.call(receiver, 1)).toThrow(TypeError);
        }
    });

    test("size must be an integral number without coercion", () => {
        for (const size of [
            undefined,
            null,
            true,
            "2",
            Symbol(),
            2n,
            NaN,
            Infinity,
            -Infinity,
            1.5,
            -0.5,
            { valueOf: () => 2 },
        ]) {
            expect(() => [].values().chunks(size)).toThrowWithMessage(TypeError, "chunkSize is not an integral Number");
        }
    });

    test("size must be in the array length range", () => {
        for (const size of [0, -0, -1, 2 ** 32, Number.MAX_VALUE]) {
            expect(() => [].values().chunks(size)).toThrowWithMessage(
                RangeError,
                "chunkSize must be between 1 and 2^32-1 inclusive"
            );
        }
    });

    test("validation closes the iterator before reading next and preserves the original error", () => {
        for (const [size, error] of [
            [undefined, TypeError],
            [0, RangeError],
        ]) {
            let returnCalls = 0;
            const source = {
                get next() {
                    expect().fail("Validation must precede reading next");
                },
                return() {
                    ++returnCalls;
                    throw new Error("close failed");
                },
            };
            expect(() => Iterator.prototype.chunks.call(source, size)).toThrow(error);
            expect(returnCalls).toBe(1);
        }
    });

    test("iterator protocol failures do not close the source", () => {
        const failure = new Error("iteration failed");
        for (const next of [
            () => {
                throw failure;
            },
            () => 1,
            () => ({
                get done() {
                    throw failure;
                },
            }),
            () => ({
                get value() {
                    throw failure;
                },
            }),
        ]) {
            const source = {
                next,
                return() {
                    expect().fail("A protocol failure must not close the source");
                },
            };
            const iterator = Iterator.prototype.chunks.call(source, 2);
            expect(() => iterator.next()).toThrow();
            expect(iterator.next()).toEqual({ value: undefined, done: true });
            iterator.return();
        }
    });

    test("return propagates errors from the source", () => {
        for (const started of [false, true]) {
            const source = {
                next: () => ({ value: 1, done: false }),
                return() {
                    throw new Error("close failed");
                },
            };
            const iterator = Iterator.prototype.chunks.call(source, 1);
            if (started) iterator.next();
            expect(() => iterator.return()).toThrowWithMessage(Error, "close failed");
        }
    });

    test("helpers cannot be resumed while executing", () => {
        const source = {
            next() {
                expect(() => iterator.next()).toThrow(TypeError);
                expect(() => iterator.return()).toThrow(TypeError);
                return { value: 1, done: false };
            },
        };
        const iterator = Iterator.prototype.chunks.call(source, 1);
        expect(iterator.next()).toEqual({ value: [1], done: false });
    });
});
