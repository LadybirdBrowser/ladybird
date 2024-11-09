describe("errors", () => {
    test("called with non-numeric object", () => {
        expect(() => {
            Iterator.prototype.take(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });

    test("called with invalid numbers", () => {
        expect(() => {
            Iterator.prototype.take(NaN);
        }).toThrowWithMessage(RangeError, "limit must not be NaN");

        expect(() => {
            Iterator.prototype.take(-1);
        }).toThrowWithMessage(RangeError, "limit must not be negative");
    });

    test("iterator's next method throws", () => {
        function TestError() {}

        class TestIterator extends Iterator {
            next() {
                throw new TestError();
            }
        }

        expect(() => {
            const iterator = new TestIterator().take(1);
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
            const iterator = new TestIterator().take(1);
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
            const iterator = new TestIterator().take(1);
            iterator.return();
        }).toThrow(TestError);
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Iterator.prototype.take).toHaveLength(1);
    });

    test("limited to zero", () => {
        function* generator() {
            yield "a";
            yield "b";
        }

        const iterator = generator().take(0);

        let value = iterator.next();
        expect(value.value).toBeUndefined();
        expect(value.done).toBeTrue();

        value = iterator.next();
        expect(value.value).toBeUndefined();
        expect(value.done).toBeTrue();
    });

    test("lower limit than the number of values", () => {
        function* generator() {
            yield "a";
            yield "b";
            yield "c";
        }

        const iterator = generator().take(2);

        let value = iterator.next();
        expect(value.value).toBe("a");
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBe("b");
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBeUndefined();
        expect(value.done).toBeTrue();
    });

    test("higher limit than the number of values", () => {
        function* generator() {
            yield "a";
            yield "b";
            yield "c";
        }

        const iterator = generator().take(Infinity);

        let value = iterator.next();
        expect(value.value).toBe("a");
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBe("b");
        expect(value.done).toBeFalse();

        value = iterator.next();
        expect(value.value).toBe("c");
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

        const iterator = new TestIterator().take(1);
        expect(returnCount).toBe(0);

        iterator.return();
        expect(returnCount).toBe(1);

        iterator.return();
        expect(returnCount).toBe(1);
    });
});
