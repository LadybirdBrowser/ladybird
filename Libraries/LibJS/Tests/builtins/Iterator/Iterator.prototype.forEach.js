describe("errors", () => {
    test("called with non-callable object", () => {
        expect(() => {
            Iterator.prototype.forEach(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "fn is not a function");
    });

    test("iterator's next method throws", () => {
        function TestError() {}

        class TestIterator extends Iterator {
            next() {
                throw new TestError();
            }
        }

        expect(() => {
            new TestIterator().forEach(() => 0);
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
            new TestIterator().forEach(() => 0);
        }).toThrow(TestError);
    });

    test("for-each function throws", () => {
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
            new TestIterator().forEach(() => {
                throw new TestError();
            });
        }).toThrow(TestError);
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Iterator.prototype.forEach).toHaveLength(1);
    });

    test("for-each function sees every value", () => {
        function* generator() {
            yield "a";
            yield "b";
        }

        let count = 0;

        generator().forEach((value, index) => {
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
});
