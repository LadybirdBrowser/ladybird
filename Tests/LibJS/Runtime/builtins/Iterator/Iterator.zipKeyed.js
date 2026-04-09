describe("errors", () => {
    test("called with non-Object", () => {
        expect(() => {
            Iterator.zipKeyed(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not an object");

        expect(() => {
            Iterator.zipKeyed({}, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Options is not an object");
    });

    test("mode is not valid", () => {
        expect(() => {
            Iterator.zipKeyed([], { mode: Symbol.hasInstance });
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not a valid value for option mode");
        expect(() => {
            Iterator.zipKeyed([], { mode: "foo" });
        }).toThrowWithMessage(TypeError, "foo is not a valid value for option mode");
    });

    test("padding is not valid", () => {
        expect(() => {
            Iterator.zipKeyed([], { mode: "longest", padding: Symbol.hasInstance });
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not a valid value for option padding");
    });

    test("@@iterator is not callable", () => {
        const iterable = {};
        iterable[Symbol.iterator] = 12389;

        expect(() => {
            Iterator.zipKeyed([iterable]);
        }).toThrowWithMessage(TypeError, "12389 is not a function");
    });

    test("@@iterator throws an exception", () => {
        function TestError() {}

        const iterable = {};
        iterable[Symbol.iterator] = () => {
            throw new TestError();
        };

        expect(() => {
            Iterator.zipKeyed([iterable]);
        }).toThrow(TestError);
    });

    test("@@iterator returns a non-Object", () => {
        const iterable = {};
        iterable[Symbol.iterator] = () => {
            return Symbol.hasInstance;
        };

        expect(() => {
            Iterator.zipKeyed([iterable]);
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not an object");
    });

    test("strict mode with unbalanced iterator values", () => {
        expect(() => {
            Iterator.zipKeyed({ a: [0], b: [] }, { mode: "strict" }).toArray();
        }).toThrowWithMessage(TypeError, "Not enough iterator results in 'strict' mode");

        expect(() => {
            Iterator.zipKeyed({ a: [], b: [2] }, { mode: "strict" }).toArray();
        }).toThrowWithMessage(TypeError, "Not enough iterator results in 'strict' mode");

        expect(() => {
            Iterator.zipKeyed({ a: [0, 1], b: [2] }, { mode: "strict" }).toArray();
        }).toThrowWithMessage(TypeError, "Not enough iterator results in 'strict' mode");

        expect(() => {
            Iterator.zipKeyed({ a: [0], b: [2, 3] }, { mode: "strict" }).toArray();
        }).toThrowWithMessage(TypeError, "Not enough iterator results in 'strict' mode");
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Iterator.zipKeyed).toHaveLength(1);
    });

    let result;

    test("mode=shortest", () => {
        result = Iterator.zipKeyed({}, { mode: "shortest" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zipKeyed({ a: [] }, { mode: "shortest" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zipKeyed({ a: [0] }, { mode: "shortest" });
        expect(result.toArray()).toEqual([{ a: 0 }]);

        result = Iterator.zipKeyed({ a: [0], b: [] }, { mode: "shortest" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zipKeyed({ a: [0], b: [2] }, { mode: "shortest" });
        expect(result.toArray()).toEqual([{ a: 0, b: 2 }]);

        result = Iterator.zipKeyed({ a: [0], b: [2, 3] }, { mode: "shortest" });
        expect(result.toArray()).toEqual([{ a: 0, b: 2 }]);

        result = Iterator.zipKeyed({ a: [0, 1], b: [2] }, { mode: "shortest" });
        expect(result.toArray()).toEqual([{ a: 0, b: 2 }]);

        result = Iterator.zipKeyed({ a: [0, 1], b: [2, 3] }, { mode: "shortest" });
        expect(result.toArray()).toEqual([
            { a: 0, b: 2 },
            { a: 1, b: 3 },
        ]);
    });

    test("mode=longest", () => {
        result = Iterator.zipKeyed({}, { mode: "longest" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zipKeyed({ a: [] }, { mode: "longest" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zipKeyed({ a: [0] }, { mode: "longest" });
        expect(result.toArray()).toEqual([{ a: 0 }]);

        result = Iterator.zipKeyed({ a: [0], b: [] }, { mode: "longest" });
        expect(result.toArray()).toEqual([{ a: 0, b: undefined }]);

        result = Iterator.zipKeyed({ a: [0], b: [] }, { mode: "longest", padding: { b: 12389 } });
        expect(result.toArray()).toEqual([{ a: 0, b: 12389 }]);

        result = Iterator.zipKeyed({ a: [0], b: [2] }, { mode: "longest" });
        expect(result.toArray()).toEqual([{ a: 0, b: 2 }]);

        result = Iterator.zipKeyed({ a: [0], b: [2, 3] }, { mode: "longest" });
        expect(result.toArray()).toEqual([
            { a: 0, b: 2 },
            { a: undefined, b: 3 },
        ]);

        result = Iterator.zipKeyed({ a: [0], b: [2, 3] }, { mode: "longest", padding: { a: 12389 } });
        expect(result.toArray()).toEqual([
            { a: 0, b: 2 },
            { a: 12389, b: 3 },
        ]);

        result = Iterator.zipKeyed({ a: [0, 1], b: [2] }, { mode: "longest" });
        expect(result.toArray()).toEqual([
            { a: 0, b: 2 },
            { a: 1, b: undefined },
        ]);

        result = Iterator.zipKeyed({ a: [0, 1], b: [2] }, { mode: "longest", padding: { b: 12389 } });
        expect(result.toArray()).toEqual([
            { a: 0, b: 2 },
            { a: 1, b: 12389 },
        ]);

        result = Iterator.zipKeyed({ a: [0, 1], b: [2, 3] }, { mode: "longest" });
        expect(result.toArray()).toEqual([
            { a: 0, b: 2 },
            { a: 1, b: 3 },
        ]);
    });

    test("mode=strict", () => {
        result = Iterator.zipKeyed({}, { mode: "strict" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zipKeyed({ a: [] }, { mode: "strict" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zipKeyed({ a: [0] }, { mode: "strict" });
        expect(result.toArray()).toEqual([{ a: 0 }]);

        result = Iterator.zipKeyed({ a: [0], b: [2] }, { mode: "strict" });
        expect(result.toArray()).toEqual([{ a: 0, b: 2 }]);

        result = Iterator.zipKeyed({ a: [0, 1], b: [2, 3] }, { mode: "strict" });
        expect(result.toArray()).toEqual([
            { a: 0, b: 2 },
            { a: 1, b: 3 },
        ]);
    });
});
