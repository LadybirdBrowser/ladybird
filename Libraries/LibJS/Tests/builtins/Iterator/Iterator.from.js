describe("errors", () => {
    test("called with non-Object", () => {
        expect(() => {
            Iterator.from(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not a string");
    });

    test("@@iterator is not callable", () => {
        const iterable = {};
        iterable[Symbol.iterator] = 12389;

        expect(() => {
            Iterator.from(iterable);
        }).toThrowWithMessage(TypeError, "12389 is not a function");
    });

    test("@@iterator throws an exception", () => {
        function TestError() {}

        const iterable = {};
        iterable[Symbol.iterator] = () => {
            throw new TestError();
        };

        expect(() => {
            Iterator.from(iterable);
        }).toThrow(TestError);
    });

    test("@@iterator return a non-Object", () => {
        const iterable = {};
        iterable[Symbol.iterator] = () => {
            return Symbol.hasInstance;
        };

        expect(() => {
            Iterator.from(iterable);
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not an object");
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Iterator.from).toHaveLength(1);
    });

    test("create Iterator from a string", () => {
        const iterator = Iterator.from("ab");

        let result = iterator.next();
        expect(result.value).toBe("a");
        expect(result.done).toBeFalse();

        result = iterator.next();
        expect(result.value).toBe("b");
        expect(result.done).toBeFalse();

        result = iterator.next();
        expect(result.value).toBeUndefined();
        expect(result.done).toBeTrue();
    });

    test("does not coerce strings to objects", () => {
        const stringIterator = String.prototype[Symbol.iterator];
        let observedType = null;

        Object.defineProperty(String.prototype, Symbol.iterator, {
            get() {
                "use strict";
                observedType = typeof this;
                return stringIterator;
            },
        });

        Iterator.from("ab");
        expect(observedType).toBe("string");
    });

    test("create Iterator from generator", () => {
        function* generator() {
            yield 1;
            yield 2;
        }

        const iterator = Iterator.from(generator());

        let result = iterator.next();
        expect(result.value).toBe(1);
        expect(result.done).toBeFalse();

        result = iterator.next();
        expect(result.value).toBe(2);
        expect(result.done).toBeFalse();

        result = iterator.next();
        expect(result.value).toBeUndefined();
        expect(result.done).toBeTrue();
    });

    test("create Iterator from iterator-like object", () => {
        class TestIterator {
            next() {
                if (this.#first) {
                    this.#first = false;
                    return { value: 1, done: false };
                }

                return { value: undefined, done: true };
            }

            #first = true;
        }

        const testIterator = new TestIterator();
        const iterator = Iterator.from(testIterator);

        let result = iterator.next();
        expect(result.value).toBe(1);
        expect(result.done).toBeFalse();

        result = iterator.next();
        expect(result.value).toBeUndefined();
        expect(result.done).toBeTrue();
    });
});
