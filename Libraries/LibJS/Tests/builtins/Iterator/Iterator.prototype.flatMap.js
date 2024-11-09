describe("errors", () => {
    test("called with non-callable object", () => {
        expect(() => {
            Iterator.prototype.flatMap(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "mapper is not a function");
    });

    test("iterator's next method throws", () => {
        function TestError() {}

        class TestIterator extends Iterator {
            next() {
                throw new TestError();
            }
        }

        expect(() => {
            const iterator = new TestIterator().flatMap(() => 0);
            iterator.next();
        }).toThrow(TestError);
    });

    test("value returned by iterator's next method throws", () => {
        function TestError() {}

        class TestIterator extends Iterator {
            next() {
                return {
                    done: false,
                    get value() {
                        throw new TestError();
                    },
                };
            }
        }

        expect(() => {
            const iterator = new TestIterator().flatMap(() => 0);
            iterator.next();
        }).toThrow(TestError);
    });

    test("iterator's return method throws", () => {
        function TestError() {}

        class TestIterator extends Iterator {
            next() {
                return {
                    done: false,
                    value: 1,
                };
            }

            return() {
                throw new TestError();
            }
        }

        expect(() => {
            const iterator = new TestIterator().flatMap(() => 0);
            iterator.return();
        }).toThrow(TestError);
    });

    test("mapper function throws", () => {
        function TestError() {}

        class TestIterator extends Iterator {
            next() {
                return {
                    done: false,
                    value: 1,
                };
            }
        }

        expect(() => {
            const iterator = new TestIterator().flatMap(() => {
                throw new TestError();
            });
            iterator.next();
        }).toThrow(TestError);
    });

    test("inner mapper value is not an object", () => {
        function* generator() {
            yield "Well hello";
            yield "friends :^)";
        }

        expect(() => {
            const iterator = generator().flatMap(() => Symbol.hasInstance);
            iterator.next();
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not an object");
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Iterator.prototype.flatMap).toHaveLength(1);
    });

    test("mapper function sees every value", () => {
        function* generator() {
            yield "Well hello";
            yield "friends :^)";
        }

        let count = 0;

        const iterator = generator().flatMap((value, index) => {
            ++count;

            switch (index) {
                case 0:
                    expect(value).toBe("Well hello");
                    break;
                case 1:
                    expect(value).toBe("friends :^)");
                    break;
                default:
                    expect().fail(`Unexpected mapper invocation: value=${value} index=${index}`);
                    break;
            }

            return value.split(" ").values();
        });

        for (const i of iterator) {
        }

        expect(count).toBe(2);
    });

    test("inner values are yielded one at a time", () => {
        function* generator() {
            yield "Well hello";
            yield "friends :^)";
        }

        const iterator = generator().flatMap(value => value.split(" ").values());

        let value = iterator.next();
        expect(value.value).toBe("Well");
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBe("hello");
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBe("friends");
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBe(":^)");
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBeUndefined();
        expect(value.done).toBeTrue();
    });

    test("return is forwarded to the underlying iterator's return method", () => {
        let returnCount = 0;

        class TestIterator extends Iterator {
            next() {
                return {
                    done: false,
                    value: 1,
                };
            }

            return() {
                ++returnCount;
                return {};
            }
        }

        const iterator = new TestIterator().flatMap(() => 0);
        expect(returnCount).toBe(0);

        iterator.return();
        expect(returnCount).toBe(1);

        iterator.return();
        expect(returnCount).toBe(1);
    });
});
