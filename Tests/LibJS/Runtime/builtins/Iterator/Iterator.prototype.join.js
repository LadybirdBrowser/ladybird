describe("errors", () => {
    test("called with non-stringable object", () => {
        expect(() => {
            Iterator.prototype.join(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to string");
    });

    test("argument validation closes underlying iterator", () => {
        let closed = false;
        let iterator = {
            __proto__: Iterator.prototype,

            return() {
                closed = true;
                return {};
            },
        };

        expect(() => {
            iterator.join(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to string");

        expect(closed).toBeTrue();
    });

    test("iterator's next method throws", () => {
        function TestError() {}

        class TestIterator extends Iterator {
            next() {
                throw new TestError();
            }
        }

        expect(() => {
            new TestIterator().join();
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
            new TestIterator().join();
        }).toThrow(TestError);
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Iterator.prototype.join).toHaveLength(1);
    });

    test("join with iterator", () => {
        expect(Iterator.from([]).join()).toBe("");
        expect(Iterator.from([]).join(" - ")).toBe("");

        const array = [1, 2, "a", "b"];
        expect(Iterator.from(array).join()).toBe("1,2,a,b");
        expect(Iterator.from(array).join(" - ")).toBe("1 - 2 - a - b");
    });

    test("join with generator", () => {
        function* empty() {}

        expect(empty().join()).toBe("");
        expect(empty().join(" - ")).toBe("");

        function* generator() {
            yield "a";
            yield "b";
            yield 1;
            yield 2;
        }

        expect(generator().join()).toBe("a,b,1,2");
        expect(generator().join(" - ")).toBe("a - b - 1 - 2");
    });

    test("nullish values", () => {
        const array = [1, null, "a", undefined, "b"];
        expect(Iterator.from(array).join()).toBe("1,,a,,b");
        expect(Iterator.from(array).join(" - ")).toBe("1 -  - a -  - b");
    });

    test("separator coerced to string", () => {
        const coercible = {
            toString: () => "::",
        };

        const array = [1, 2, "a", "b"];
        expect(Iterator.from(array).join(coercible)).toBe("1::2::a::b");
    });

    test("values coerced to string", () => {
        const coercible = {
            toString: () => "foo",
        };

        const array = [1, coercible, "a", "b"];
        expect(Iterator.from(array).join()).toBe("1,foo,a,b");
        expect(Iterator.from(array).join(" - ")).toBe("1 - foo - a - b");
    });
});
