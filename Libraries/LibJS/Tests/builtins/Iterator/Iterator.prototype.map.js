describe("errors", () => {
    test("called with non-callable object", () => {
        expect(() => {
            Iterator.prototype.map(Symbol.hasInstance);
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
            const iterator = new TestIterator().map(() => 0);
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
            const iterator = new TestIterator().map(() => 0);
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
            const iterator = new TestIterator().map(() => 0);
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
            const iterator = new TestIterator().map(() => {
                throw new TestError();
            });
            iterator.next();
        }).toThrow(TestError);
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Iterator.prototype.map).toHaveLength(1);
    });

    test("mapper function sees every value", () => {
        function* generator() {
            yield "a";
            yield "b";
        }

        let count = 0;

        const iterator = generator().map((value, index) => {
            ++count;

            switch (index) {
                case 0:
                    expect(value).toBe("a");
                    break;
                case 1:
                    expect(value).toBe("b");
                    break;
                default:
                    expect().fail(`Unexpected mapper invocation: value=${value} index=${index}`);
                    break;
            }

            return value;
        });

        for (const i of iterator) {
        }

        expect(count).toBe(2);
    });

    test("mapper function can modify values", () => {
        function* generator() {
            yield "a";
            yield "b";
        }

        const iterator = generator().map(value => value.toUpperCase());

        let value = iterator.next();
        expect(value.value).toBe("A");
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBe("B");
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBeUndefined();
        expect(value.done).toBeTrue();
    });

    test("mappers can be chained", () => {
        function* generator() {
            yield 1;
            yield 2;
        }

        const iterator = generator()
            .map(value => value * 2)
            .map(value => value + 10);

        let value = iterator.next();
        expect(value.value).toBe(12);
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBe(14);
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

        const iterator = new TestIterator().map(() => 0);
        expect(returnCount).toBe(0);

        iterator.return();
        expect(returnCount).toBe(1);

        iterator.return();
        expect(returnCount).toBe(1);
    });
});
