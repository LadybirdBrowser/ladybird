describe("errors", () => {
    test("called with non-callable object", () => {
        expect(() => {
            Iterator.prototype.find(Symbol.hasInstance);
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
            new TestIterator().find(() => 0);
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
            new TestIterator().find(() => 0);
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
            new TestIterator().find(() => {
                throw new TestError();
            });
        }).toThrow(TestError);
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Iterator.prototype.find).toHaveLength(1);
    });

    test("predicate function can see every value", () => {
        function* generator() {
            yield "a";
            yield "b";
        }

        let count = 0;

        const result = generator().find((value, index) => {
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

            return false;
        });

        expect(count).toBe(2);
        expect(result).toBeUndefined();
    });

    test("iteration stops when predicate returns true", () => {
        function* generator() {
            yield "a";
            yield "b";
            yield "c";
        }

        let count = 0;

        const result = generator().find(value => {
            ++count;
            return value === "b";
        });

        expect(count).toBe(2);
        expect(result).toBe("b");
    });
});
