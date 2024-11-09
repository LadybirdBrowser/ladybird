describe("errors", () => {
    test("iterator's next method throws", () => {
        function TestError() {}

        class TestIterator extends Iterator {
            next() {
                throw new TestError();
            }
        }

        expect(() => {
            new TestIterator().toArray();
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
            new TestIterator().toArray();
        }).toThrow(TestError);
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Iterator.prototype.toArray).toHaveLength(0);
    });

    test("empty list", () => {
        function* generator() {}

        const result = generator().toArray();
        expect(result).toEqual([]);
    });

    test("non-empty list", () => {
        function* generator() {
            yield 1;
            yield 2;
            yield 3;
        }

        const result = generator().toArray();
        expect(result).toEqual([1, 2, 3]);
    });
});
