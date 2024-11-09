describe("errors", () => {
    test("called with non-callable object", () => {
        expect(() => {
            Iterator.prototype.filter(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "predicate is not a function");
    });

    test("iterator's next method throws", () => {
        function TestError() {}

        class TestIterator extends Iterator {
            next() {
                throw new TestError();
            }
        }

        expect(() => {
            const iterator = new TestIterator().filter(() => true);
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
            const iterator = new TestIterator().filter(() => true);
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
            const iterator = new TestIterator().filter(() => true);
            iterator.return();
        }).toThrow(TestError);
    });

    test("predicate function throws", () => {
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
            const iterator = new TestIterator().filter(() => {
                throw new TestError();
            });
            iterator.next();
        }).toThrow(TestError);
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Iterator.prototype.filter).toHaveLength(1);
    });

    test("predicate function sees every value", () => {
        function* generator() {
            yield "a";
            yield "b";
        }

        let count = 0;

        const iterator = generator().filter((value, index) => {
            ++count;

            switch (index) {
                case 0:
                    expect(value).toBe("a");
                    break;
                case 1:
                    expect(value).toBe("b");
                    break;
                default:
                    expect().fail(`Unexpected predicate invocation: value=${value} index=${index}`);
                    break;
            }

            return true;
        });

        for (const i of iterator) {
        }

        expect(count).toBe(2);
    });

    test("predicate function can select values", () => {
        function* generator() {
            yield 1;
            yield 0;
            yield 2;
            yield 0;
        }

        const iterator = generator().filter(value => value > 0);

        let value = iterator.next();
        expect(value.value).toBe(1);
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBe(2);
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBeUndefined();
        expect(value.done).toBeTrue();
    });

    test("predicates can be chained", () => {
        function* generator() {
            yield 1;
            yield 0;
            yield 2;
            yield 0;
        }

        let firstFilterCount = 0;
        let secondFilterCount = 0;

        const iterator = generator()
            .filter(value => {
                ++firstFilterCount;
                return value > 0;
            })
            .filter(value => {
                ++secondFilterCount;
                return value > 1;
            });

        let value = iterator.next();
        expect(value.value).toBe(2);
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBeUndefined();
        expect(value.done).toBeTrue();

        expect(firstFilterCount).toBe(4);
        expect(secondFilterCount).toBe(2);
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

        const iterator = new TestIterator().filter(() => true);
        expect(returnCount).toBe(0);

        iterator.return();
        expect(returnCount).toBe(1);

        iterator.return();
        expect(returnCount).toBe(1);
    });
});
