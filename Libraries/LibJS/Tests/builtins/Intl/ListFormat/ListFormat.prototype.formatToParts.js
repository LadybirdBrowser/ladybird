describe("errors", () => {
    function SomeError() {}

    test("called on non-ListFormat object", () => {
        expect(() => {
            Intl.ListFormat.prototype.formatToParts([]);
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.ListFormat");
    });

    test("called with non-string iterable", () => {
        expect(() => {
            new Intl.ListFormat().formatToParts([1]);
        }).toThrowWithMessage(TypeError, "1 is not a string");
    });

    test("called with iterable that throws immediately", () => {
        let iterable = {
            [Symbol.iterator]() {
                throw new SomeError();
            },
        };

        expect(() => {
            new Intl.ListFormat().formatToParts(iterable);
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
            new Intl.ListFormat().formatToParts(iterable);
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
            new Intl.ListFormat().formatToParts(iterable);
        }).toThrow(SomeError);
    });
});

describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Intl.ListFormat.prototype.formatToParts).toHaveLength(1);
    });

    test("undefined list returns empty string", () => {
        expect(new Intl.ListFormat().formatToParts(undefined)).toEqual([]);
    });
});

describe("type=conjunction", () => {
    test("style=long", () => {
        let en = new Intl.ListFormat("en", { type: "conjunction", style: "long" });
        expect(en.formatToParts(["a"])).toEqual([{ type: "element", value: "a" }]);
        expect(en.formatToParts(["a", "b"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: " and " },
            { type: "element", value: "b" },
        ]);
        expect(en.formatToParts(["a", "b", "c"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: ", " },
            { type: "element", value: "b" },
            { type: "literal", value: ", and " },
            { type: "element", value: "c" },
        ]);
    });

    test("style=short", () => {
        let en = new Intl.ListFormat("en", { type: "conjunction", style: "short" });
        expect(en.formatToParts(["a"])).toEqual([{ type: "element", value: "a" }]);
        expect(en.formatToParts(["a", "b"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: " & " },
            { type: "element", value: "b" },
        ]);
        expect(en.formatToParts(["a", "b", "c"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: ", " },
            { type: "element", value: "b" },
            { type: "literal", value: ", & " },
            { type: "element", value: "c" },
        ]);
    });

    test("style=narrow", () => {
        let en = new Intl.ListFormat("en", { type: "conjunction", style: "narrow" });
        expect(en.formatToParts(["a"])).toEqual([{ type: "element", value: "a" }]);
        expect(en.formatToParts(["a", "b"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: ", " },
            { type: "element", value: "b" },
        ]);
        expect(en.formatToParts(["a", "b", "c"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: ", " },
            { type: "element", value: "b" },
            { type: "literal", value: ", " },
            { type: "element", value: "c" },
        ]);
    });
});

describe("type=disjunction", () => {
    test("style=long", () => {
        let en = new Intl.ListFormat("en", { type: "disjunction", style: "long" });
        expect(en.formatToParts(["a"])).toEqual([{ type: "element", value: "a" }]);
        expect(en.formatToParts(["a", "b"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: " or " },
            { type: "element", value: "b" },
        ]);
        expect(en.formatToParts(["a", "b", "c"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: ", " },
            { type: "element", value: "b" },
            { type: "literal", value: ", or " },
            { type: "element", value: "c" },
        ]);
    });

    test("style=short", () => {
        let en = new Intl.ListFormat("en", { type: "disjunction", style: "short" });
        expect(en.formatToParts(["a"])).toEqual([{ type: "element", value: "a" }]);
        expect(en.formatToParts(["a", "b"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: " or " },
            { type: "element", value: "b" },
        ]);
        expect(en.formatToParts(["a", "b", "c"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: ", " },
            { type: "element", value: "b" },
            { type: "literal", value: ", or " },
            { type: "element", value: "c" },
        ]);
    });

    test("style=narrow", () => {
        let en = new Intl.ListFormat("en", { type: "disjunction", style: "narrow" });
        expect(en.formatToParts(["a"])).toEqual([{ type: "element", value: "a" }]);
        expect(en.formatToParts(["a", "b"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: " or " },
            { type: "element", value: "b" },
        ]);
        expect(en.formatToParts(["a", "b", "c"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: ", " },
            { type: "element", value: "b" },
            { type: "literal", value: ", or " },
            { type: "element", value: "c" },
        ]);
    });
});

describe("type=unit", () => {
    test("style=long", () => {
        let en = new Intl.ListFormat("en", { type: "unit", style: "long" });
        expect(en.formatToParts(["a"])).toEqual([{ type: "element", value: "a" }]);
        expect(en.formatToParts(["a", "b"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: ", " },
            { type: "element", value: "b" },
        ]);
        expect(en.formatToParts(["a", "b", "c"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: ", " },
            { type: "element", value: "b" },
            { type: "literal", value: ", " },
            { type: "element", value: "c" },
        ]);
    });

    test("style=short", () => {
        let en = new Intl.ListFormat("en", { type: "unit", style: "short" });
        expect(en.formatToParts(["a"])).toEqual([{ type: "element", value: "a" }]);
        expect(en.formatToParts(["a", "b"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: ", " },
            { type: "element", value: "b" },
        ]);
        expect(en.formatToParts(["a", "b", "c"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: ", " },
            { type: "element", value: "b" },
            { type: "literal", value: ", " },
            { type: "element", value: "c" },
        ]);
    });

    test("style=narrow", () => {
        let en = new Intl.ListFormat("en", { type: "unit", style: "narrow" });
        expect(en.formatToParts(["a"])).toEqual([{ type: "element", value: "a" }]);
        expect(en.formatToParts(["a", "b"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: " " },
            { type: "element", value: "b" },
        ]);
        expect(en.formatToParts(["a", "b", "c"])).toEqual([
            { type: "element", value: "a" },
            { type: "literal", value: " " },
            { type: "element", value: "b" },
            { type: "literal", value: " " },
            { type: "element", value: "c" },
        ]);
    });
});
