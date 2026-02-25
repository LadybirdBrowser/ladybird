describe("errors", () => {
    test("called with non-Object", () => {
        expect(() => {
            Iterator.zip(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not an object");

        expect(() => {
            Iterator.zip({}, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Options is not an object");
    });

    test("mode is not valid", () => {
        expect(() => {
            Iterator.zip([], { mode: Symbol.hasInstance });
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not a valid value for option mode");
        expect(() => {
            Iterator.zip([], { mode: "foo" });
        }).toThrowWithMessage(TypeError, "foo is not a valid value for option mode");
    });

    test("padding is not valid", () => {
        expect(() => {
            Iterator.zip([], { mode: "longest", padding: Symbol.hasInstance });
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not a valid value for option padding");
    });

    test("@@iterator is not callable", () => {
        const iterable = {};
        iterable[Symbol.iterator] = 12389;

        expect(() => {
            Iterator.zip([iterable]);
        }).toThrowWithMessage(TypeError, "12389 is not a function");
    });

    test("@@iterator throws an exception", () => {
        function TestError() {}

        const iterable = {};
        iterable[Symbol.iterator] = () => {
            throw new TestError();
        };

        expect(() => {
            Iterator.zip([iterable]);
        }).toThrow(TestError);
    });

    test("@@iterator returns a non-Object", () => {
        const iterable = {};
        iterable[Symbol.iterator] = () => {
            return Symbol.hasInstance;
        };

        expect(() => {
            Iterator.zip([iterable]);
        }).toThrowWithMessage(TypeError, "Symbol(Symbol.hasInstance) is not an object");
    });

    test("strict mode with unbalanced iterator values", () => {
        expect(() => {
            Iterator.zip([[0], []], { mode: "strict" }).toArray();
        }).toThrowWithMessage(TypeError, "Not enough iterator results in 'strict' mode");

        expect(() => {
            Iterator.zip([[], [2]], { mode: "strict" }).toArray();
        }).toThrowWithMessage(TypeError, "Not enough iterator results in 'strict' mode");

        expect(() => {
            Iterator.zip([[0, 1], [2]], { mode: "strict" }).toArray();
        }).toThrowWithMessage(TypeError, "Not enough iterator results in 'strict' mode");

        expect(() => {
            Iterator.zip([[0], [2, 3]], { mode: "strict" }).toArray();
        }).toThrowWithMessage(TypeError, "Not enough iterator results in 'strict' mode");
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Iterator.zip).toHaveLength(1);
    });

    let result;

    test("mode=shortest", () => {
        result = Iterator.zip([], { mode: "shortest" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zip([[]], { mode: "shortest" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zip([[0]], { mode: "shortest" });
        expect(result.toArray()).toEqual([[0]]);

        result = Iterator.zip([[0], []], { mode: "shortest" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zip([[0], [2]], { mode: "shortest" });
        expect(result.toArray()).toEqual([[0, 2]]);

        result = Iterator.zip([[0], [2, 3]], { mode: "shortest" });
        expect(result.toArray()).toEqual([[0, 2]]);

        result = Iterator.zip([[0, 1], [2]], { mode: "shortest" });
        expect(result.toArray()).toEqual([[0, 2]]);

        result = Iterator.zip(
            [
                [0, 1],
                [2, 3],
            ],
            { mode: "shortest" }
        );
        expect(result.toArray()).toEqual([
            [0, 2],
            [1, 3],
        ]);
    });

    test("mode=longest", () => {
        result = Iterator.zip([], { mode: "longest" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zip([[]], { mode: "longest" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zip([[0]], { mode: "longest" });
        expect(result.toArray()).toEqual([[0]]);

        result = Iterator.zip([[0], []], { mode: "longest" });
        expect(result.toArray()).toEqual([[0, undefined]]);

        result = Iterator.zip([[0], []], { mode: "longest", padding: [undefined, 12389] });
        expect(result.toArray()).toEqual([[0, 12389]]);

        result = Iterator.zip([[0], [2]], { mode: "longest" });
        expect(result.toArray()).toEqual([[0, 2]]);

        result = Iterator.zip([[0], [2, 3]], { mode: "longest" });
        expect(result.toArray()).toEqual([
            [0, 2],
            [undefined, 3],
        ]);

        result = Iterator.zip([[0], [2, 3]], { mode: "longest", padding: [12389] });
        expect(result.toArray()).toEqual([
            [0, 2],
            [12389, 3],
        ]);

        result = Iterator.zip([[0, 1], [2]], { mode: "longest" });
        expect(result.toArray()).toEqual([
            [0, 2],
            [1, undefined],
        ]);

        result = Iterator.zip([[0, 1], [2]], { mode: "longest", padding: [undefined, 12389] });
        expect(result.toArray()).toEqual([
            [0, 2],
            [1, 12389],
        ]);

        result = Iterator.zip(
            [
                [0, 1],
                [2, 3],
            ],
            { mode: "longest" }
        );
        expect(result.toArray()).toEqual([
            [0, 2],
            [1, 3],
        ]);
    });

    test("mode=strict", () => {
        result = Iterator.zip([], { mode: "strict" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zip([[]], { mode: "strict" });
        expect(result.toArray()).toEqual([]);

        result = Iterator.zip([[0]], { mode: "strict" });
        expect(result.toArray()).toEqual([[0]]);

        result = Iterator.zip([[0], [2]], { mode: "strict" });
        expect(result.toArray()).toEqual([[0, 2]]);

        result = Iterator.zip([
            [0, 1],
            [2, 3],
        ]);
        expect(result.toArray()).toEqual([
            [0, 2],
            [1, 3],
        ]);
    });
});
