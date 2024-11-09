describe("errors", () => {
    test("called with non-callable object", () => {
        expect(() => {
            Iterator.prototype.every(Symbol.hasInstance);
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
            new TestIterator().every(() => 0);
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
            new TestIterator().every(() => 0);
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
            new TestIterator().every(() => {
                throw new TestError();
            });
        }).toThrow(TestError);
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Iterator.prototype.every).toHaveLength(1);
    });

    test("predicate function can see every value", () => {
        function* generator() {
            yield "a";
            yield "b";
        }

        let count = 0;

        const result = generator().every((value, index) => {
            ++count;

            switch (index) {
                case 0:
                    expect(value).toBe("a");
                    break;
                case 1:
                    expect(value).toBe("b");
                    break;
                default:
                    expect().fail(`Unexpected reducer invocation: value=${value} index=${index}`);
                    break;
            }

            return true;
        });

        expect(count).toBe(2);
        expect(result).toBeTrue();
    });

    test("iteration stops when predicate returns false", () => {
        function* generator() {
            yield "a";
            yield "b";
            yield "c";
        }

        let count = 0;

        const result = generator().every(value => {
            ++count;
            return value === "a";
        });

        expect(count).toBe(2);
        expect(result).toBeFalse();
    });
});
