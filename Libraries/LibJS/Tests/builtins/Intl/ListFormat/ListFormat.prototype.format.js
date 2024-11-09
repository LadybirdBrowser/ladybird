describe("errors", () => {
    function SomeError() {}

    test("called on non-ListFormat object", () => {
        expect(() => {
            Intl.ListFormat.prototype.format([]);
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.ListFormat");
    });

    test("called with non-string iterable", () => {
        expect(() => {
            new Intl.ListFormat().format([1]);
        }).toThrowWithMessage(TypeError, "1 is not a string");
    });

    test("called with iterable that throws immediately", () => {
        let iterable = {
            [Symbol.iterator]() {
                throw new SomeError();
            },
        };

        expect(() => {
            new Intl.ListFormat().format(iterable);
        }).toThrow(SomeError);
    });

    test("called with iterable that throws on step", () => {
        let iterable = {
            [Symbol.iterator]() {
                return this;
            },
            next() {
                throw new SomeError();
            },
        };

        expect(() => {
            new Intl.ListFormat().format(iterable);
        }).toThrow(SomeError);
    });

    test("called with iterable that throws on value resolution", () => {
        let iterable = {
            [Symbol.iterator]() {
                return this;
            },
            next() {
                return {
                    done: false,
                    get value() {
                        throw new SomeError();
                    },
                };
            },
        };

        expect(() => {
            new Intl.ListFormat().format(iterable);
        }).toThrow(SomeError);
    });
});

describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Intl.ListFormat.prototype.format).toHaveLength(1);
    });

    test("undefined list returns empty string", () => {
        expect(new Intl.ListFormat().format(undefined)).toBe("");
    });
});

describe("type=conjunction", () => {
    test("style=long", () => {
        let en = new Intl.ListFormat("en", { type: "conjunction", style: "long" });
        expect(en.format(["a"])).toBe("a");
        expect(en.format(["a", "b"])).toBe("a and b");
        expect(en.format(["a", "b", "c"])).toBe("a, b, and c");

        let es = new Intl.ListFormat("es-419", { type: "conjunction", style: "long" });
        expect(es.format(["a"])).toBe("a");
        expect(es.format(["a", "b"])).toBe("a y b");
        expect(es.format(["a", "b", "c"])).toBe("a, b y c");
    });

    test("style=short", () => {
        let en = new Intl.ListFormat("en", { type: "conjunction", style: "short" });
        expect(en.format(["a"])).toBe("a");
        expect(en.format(["a", "b"])).toBe("a & b");
        expect(en.format(["a", "b", "c"])).toBe("a, b, & c");

        let es = new Intl.ListFormat("es-419", { type: "conjunction", style: "short" });
        expect(es.format(["a"])).toBe("a");
        expect(es.format(["a", "b"])).toBe("a y b");
        expect(es.format(["a", "b", "c"])).toBe("a, b y c");
    });

    test("style=narrow", () => {
        let en = new Intl.ListFormat("en", { type: "conjunction", style: "narrow" });
        expect(en.format(["a"])).toBe("a");
        expect(en.format(["a", "b"])).toBe("a, b");
        expect(en.format(["a", "b", "c"])).toBe("a, b, c");

        let es = new Intl.ListFormat("es-419", { type: "conjunction", style: "narrow" });
        expect(es.format(["a"])).toBe("a");
        expect(es.format(["a", "b"])).toBe("a y b");
        expect(es.format(["a", "b", "c"])).toBe("a, b y c");
    });
});

describe("type=disjunction", () => {
    test("style=long", () => {
        let en = new Intl.ListFormat("en", { type: "disjunction", style: "long" });
        expect(en.format(["a"])).toBe("a");
        expect(en.format(["a", "b"])).toBe("a or b");
        expect(en.format(["a", "b", "c"])).toBe("a, b, or c");

        let es = new Intl.ListFormat("es-419", { type: "disjunction", style: "long" });
        expect(es.format(["a"])).toBe("a");
        expect(es.format(["a", "b"])).toBe("a o b");
        expect(es.format(["a", "b", "c"])).toBe("a, b o c");
    });

    test("style=short", () => {
        let en = new Intl.ListFormat("en", { type: "disjunction", style: "short" });
        expect(en.format(["a"])).toBe("a");
        expect(en.format(["a", "b"])).toBe("a or b");
        expect(en.format(["a", "b", "c"])).toBe("a, b, or c");

        let es = new Intl.ListFormat("es-419", { type: "disjunction", style: "short" });
        expect(es.format(["a"])).toBe("a");
        expect(es.format(["a", "b"])).toBe("a o b");
        expect(es.format(["a", "b", "c"])).toBe("a, b o c");
    });

    test("style=narrow", () => {
        let en = new Intl.ListFormat("en", { type: "disjunction", style: "narrow" });
        expect(en.format(["a"])).toBe("a");
        expect(en.format(["a", "b"])).toBe("a or b");
        expect(en.format(["a", "b", "c"])).toBe("a, b, or c");

        let es = new Intl.ListFormat("es-419", { type: "disjunction", style: "narrow" });
        expect(es.format(["a"])).toBe("a");
        expect(es.format(["a", "b"])).toBe("a o b");
        expect(es.format(["a", "b", "c"])).toBe("a, b o c");
    });
});

describe("type=unit", () => {
    test("style=long", () => {
        let en = new Intl.ListFormat("en", { type: "unit", style: "long" });
        expect(en.format(["a"])).toBe("a");
        expect(en.format(["a", "b"])).toBe("a, b");
        expect(en.format(["a", "b", "c"])).toBe("a, b, c");

        let es = new Intl.ListFormat("es-419", { type: "unit", style: "long" });
        expect(es.format(["a"])).toBe("a");
        expect(es.format(["a", "b"])).toBe("a y b");
        expect(es.format(["a", "b", "c"])).toBe("a, b y c");
    });

    test("style=short", () => {
        let en = new Intl.ListFormat("en", { type: "unit", style: "short" });
        expect(en.format(["a"])).toBe("a");
        expect(en.format(["a", "b"])).toBe("a, b");
        expect(en.format(["a", "b", "c"])).toBe("a, b, c");

        let es = new Intl.ListFormat("es-419", { type: "unit", style: "short" });
        expect(es.format(["a"])).toBe("a");
        expect(es.format(["a", "b"])).toBe("a y b");
        expect(es.format(["a", "b", "c"])).toBe("a, b, c");
    });

    test("style=narrow", () => {
        let en = new Intl.ListFormat("en", { type: "unit", style: "narrow" });
        expect(en.format(["a"])).toBe("a");
        expect(en.format(["a", "b"])).toBe("a b");
        expect(en.format(["a", "b", "c"])).toBe("a b c");

        let es = new Intl.ListFormat("es-419", { type: "unit", style: "narrow" });
        expect(es.format(["a"])).toBe("a");
        expect(es.format(["a", "b"])).toBe("a b");
        expect(es.format(["a", "b", "c"])).toBe("a b c");
    });
});
