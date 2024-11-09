describe("errors", () => {
    test("called with non-callable object", () => {
        expect(() => {
            Iterator.prototype.reduce(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "reducer is not a function");
    });

    test("iterator's next method throws", () => {
        function TestError() {}

        class TestIterator extends Iterator {
            next() {
                throw new TestError();
            }
        }

        expect(() => {
            new TestIterator().reduce(() => 0);
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
            new TestIterator().reduce(() => 0);
        }).toThrow(TestError);
    });

    test("reducer function throws", () => {
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
            new TestIterator().reduce(() => {
                throw new TestError();
            });
        }).toThrow(TestError);
    });

    test("no available initial value", () => {
        function* generator() {}

        expect(() => {
            generator().reduce(() => 0);
        }).toThrowWithMessage(TypeError, "Reduce of empty array with no initial value");
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Iterator.prototype.reduce).toHaveLength(1);
    });

    test("reducer function sees every value", () => {
        function* generator() {
            yield "a";
            yield "b";
        }

        let count = 0;

        generator().reduce((accumulator, value, index) => {
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

            return value;
        }, "");

        expect(count).toBe(2);
    });

    test("reducer uses first value as initial value", () => {
        function* generator() {
            yield 1;
            yield 2;
            yield 3;
        }

        const result = generator().reduce((accumulator, value) => accumulator + value);
        expect(result).toBe(6);
    });

    test("reducer uses provided value as initial value", () => {
        function* generator() {
            yield 1;
            yield 2;
            yield 3;
        }

        const result = generator().reduce((accumulator, value) => accumulator + value, 10);
        expect(result).toBe(16);
    });
});
