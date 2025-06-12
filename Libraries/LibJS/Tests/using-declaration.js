describe("basic usage", () => {
    test.xfail("disposes after block exit", () => {
        let disposed = false;
        let inBlock = false;
        {
            expect(disposed).toBeFalse();
            using a = { [Symbol.dispose]() { disposed = true; } };
            inBlock = true;
            expect(disposed).toBeFalse();
        }

        expect(inBlock).toBeTrue();
        expect(disposed).toBeTrue();
    });

    test.xfail("disposes in reverse order after block exit", () => {
        const disposed = [];
        {
            expect(disposed).toHaveLength(0);
            using a = { [Symbol.dispose]() { disposed.push('a'); } };
            using b = { [Symbol.dispose]() { disposed.push('b'); } };
            expect(disposed).toHaveLength(0);
        }

        expect(disposed).toEqual(['b', 'a']);
    });

    test.xfail("disposes in reverse order after block exit even in same declaration", () => {
        const disposed = [];
        {
            expect(disposed).toHaveLength(0);
            using a = { [Symbol.dispose]() { disposed.push('a'); } },
                  b = { [Symbol.dispose]() { disposed.push('b'); } };
            expect(disposed).toHaveLength(0);
        }

        expect(disposed).toEqual(['b', 'a']);
    });
});

describe("behavior with exceptions", () => {
    function ExpectedError(name) { this.name = name; }

    test.xfail("is run even after throw", () => {
        let disposed = false;
        let inBlock = false;
        let inCatch = false;
        try {
            expect(disposed).toBeFalse();
            using a = { [Symbol.dispose]() { disposed = true; } };
            inBlock = true;
            expect(disposed).toBeFalse();
            throw new ExpectedError();
            expect().fail();
        } catch (e) {
            expect(disposed).toBeTrue();
            expect(e).toBeInstanceOf(ExpectedError);
            inCatch = true;
        }
        expect(disposed).toBeTrue();
        expect(inBlock).toBeTrue();
        expect(inCatch).toBeTrue();
    });

    test.xfail("throws error if dispose method does", () => {
        let disposed = false;
        let endOfTry = false;
        let inCatch = false;
        try {
            expect(disposed).toBeFalse();
            using a = { [Symbol.dispose]() {
                    disposed = true;
                    throw new ExpectedError();
                } };
            expect(disposed).toBeFalse();
            endOfTry = true;
        } catch (e) {
            expect(disposed).toBeTrue();
            expect(e).toBeInstanceOf(ExpectedError);
            inCatch = true;
        }
        expect(disposed).toBeTrue();
        expect(endOfTry).toBeTrue();
        expect(inCatch).toBeTrue();
    });

    test.xfail("if block and using throw get suppressed error", () => {
        let disposed = false;
        let inCatch = false;
        try {
            expect(disposed).toBeFalse();
            using a = { [Symbol.dispose]() {
                    disposed = true;
                    throw new ExpectedError('dispose');
                } };
            expect(disposed).toBeFalse();
            throw new ExpectedError('throw');
        } catch (e) {
            expect(disposed).toBeTrue();
            expect(e).toBeInstanceOf(SuppressedError);
            expect(e.error).toBeInstanceOf(ExpectedError);
            expect(e.error.name).toBe('dispose');
            expect(e.suppressed).toBeInstanceOf(ExpectedError);
            expect(e.suppressed.name).toBe('throw');
            inCatch = true;
        }
        expect(disposed).toBeTrue();
        expect(inCatch).toBeTrue();
    });

    test.xfail("multiple throwing disposes give suppressed error", () => {
        let inCatch = false;
        try {
            {
                using a = { [Symbol.dispose]() {
                    throw new ExpectedError('a');
                } };

                using b = { [Symbol.dispose]() {
                    throw new ExpectedError('b');
                } };
            }

            expect().fail();
        } catch (e) {
            expect(e).toBeInstanceOf(SuppressedError);
            expect(e.error).toBeInstanceOf(ExpectedError);
            expect(e.error.name).toBe('a');
            expect(e.suppressed).toBeInstanceOf(ExpectedError);
            expect(e.suppressed.name).toBe('b');
            inCatch = true;
        }
        expect(inCatch).toBeTrue();
    });

    test.xfail("3 throwing disposes give chaining suppressed error", () => {
        let inCatch = false;
        try {
            {
                using a = { [Symbol.dispose]() {
                    throw new ExpectedError('a');
                } };

                using b = { [Symbol.dispose]() {
                    throw new ExpectedError('b');
                } };

                using c = { [Symbol.dispose]() {
                    throw new ExpectedError('c');
                } };
            }

            expect().fail();
        } catch (e) {
            expect(e).toBeInstanceOf(SuppressedError);
            expect(e.error).toBeInstanceOf(ExpectedError);
            expect(e.error.name).toBe('a');
            expect(e.suppressed).toBeInstanceOf(SuppressedError);

            const inner = e.suppressed;

            expect(inner.error).toBeInstanceOf(ExpectedError);
            expect(inner.error.name).toBe('b');
            expect(inner.suppressed).toBeInstanceOf(ExpectedError);
            expect(inner.suppressed.name).toBe('c');
            inCatch = true;
        }
        expect(inCatch).toBeTrue();
    });

    test.xfail("normal error and multiple disposing erorrs give chaining suppressed errors", () => {
        let inCatch = false;
        try {
            using a = { [Symbol.dispose]() {
                throw new ExpectedError('a');
            } };

            using b = { [Symbol.dispose]() {
                throw new ExpectedError('b');
            } };

            throw new ExpectedError('top');
        } catch (e) {
            expect(e).toBeInstanceOf(SuppressedError);
            expect(e.error).toBeInstanceOf(ExpectedError);
            expect(e.error.name).toBe('a');
            expect(e.suppressed).toBeInstanceOf(SuppressedError);

            const inner = e.suppressed;

            expect(inner.error).toBeInstanceOf(ExpectedError);
            expect(inner.error.name).toBe('b');
            expect(inner.suppressed).toBeInstanceOf(ExpectedError);
            expect(inner.suppressed.name).toBe('top');
            inCatch = true;
        }
        expect(inCatch).toBeTrue();
    });
});

describe("works in a bunch of scopes", () => {
    test.xfail("works in block", () => {
        let dispose = false;
        expect(dispose).toBeFalse();
        {
            expect(dispose).toBeFalse();
            using a = { [Symbol.dispose]() { dispose = true; } }
            expect(dispose).toBeFalse();
        }
        expect(dispose).toBeTrue();
    });

    test.xfail("works in static class block", () => {
        let dispose = false;
        expect(dispose).toBeFalse();
        class A {
            static {
                expect(dispose).toBeFalse();
                using a = { [Symbol.dispose]() { dispose = true; } }
                expect(dispose).toBeFalse();
            }
        }
        expect(dispose).toBeTrue();
    });

    test.xfail("works in function", () => {
        let dispose = [];
        function f(val) {
            const disposeLength = dispose.length;
            using a = { [Symbol.dispose]() { dispose.push(val); } }
            expect(dispose.length).toBe(disposeLength);
        }
        expect(dispose).toEqual([]);
        f(0);
        expect(dispose).toEqual([0]);
        f(1);
        expect(dispose).toEqual([0, 1]);
    });

    test.xfail("switch block is treated as full block in function", () => {
        let disposeFull = [];
        let disposeInner = false;

        function pusher(val) {
            return {
                val,
                [Symbol.dispose]() { disposeFull.push(val); }
            };
        }

        switch (2) {
            case 3:
                using notDisposed = { [Symbol.dispose]() { expect().fail("not-disposed 1"); } };
            case 2:
                expect(disposeFull).toEqual([]);
                using a = pusher('a');
                expect(disposeFull).toEqual([]);

                using b = pusher('b');
                expect(disposeFull).toEqual([]);
                expect(b.val).toBe('b');

                expect(disposeInner).toBeFalse();
                // fallthrough
            case 1: {
                expect(disposeFull).toEqual([]);
                expect(disposeInner).toBeFalse();

                using inner = { [Symbol.dispose]() { disposeInner = true; } }

                expect(disposeInner).toBeFalse();
            }
                expect(disposeInner).toBeTrue();
                using c = pusher('c');
                expect(c.val).toBe('c');
                break;
            case 0:
                using notDisposed2 = { [Symbol.dispose]() { expect().fail("not-disposed 2"); } };
        }

        expect(disposeInner).toBeTrue();
        expect(disposeFull).toEqual(['c', 'b', 'a']);
    });
});

describe("invalid using bindings", () => {
    test.xfail("nullish values do not throw", () => {
        using a = null, b = undefined;
        expect(a).toBeNull();
        expect(b).toBeUndefined();
    });

    test.xfail("non-object throws", () => {
        [0, "a", true, NaN, 4n, Symbol.dispose].forEach(value => {
            expect(() => {
                using v = value;
            }).toThrowWithMessage(TypeError, "is not an object");
        });
    });

    test.xfail("object without dispose throws", () => {
        expect(() => {
            using a = {};
        }).toThrowWithMessage(TypeError, "does not have dispose method");
    });

    test.xfail("object with non callable dispose throws", () => {
        [0, "a", true, NaN, 4n, Symbol.dispose, [], {}].forEach(value => {
            expect(() => {
                using a = { [Symbol.dispose]: value };
            }).toThrowWithMessage(TypeError, "is not a function");
        });
    });
});

describe("using is still a valid variable name", () => {
    test("var", () => {
        "use strict";
        var using = 1;
        expect(using).toBe(1);
    });

    test("const", () => {
        "use strict";
        const using = 1;
        expect(using).toBe(1);
    });

    test("let", () => {
        "use strict";
        let using = 1;
        expect(using).toBe(1);
    });

    test.xfail("using", () => {
        "use strict";
        using using = null;
        expect(using).toBeNull();
    });

    test("function", () => {
        "use strict";
        function using() { return 1; }
        expect(using()).toBe(1);
    });
});

describe("syntax errors / werid artifacts which remain valid", () => {
    test("no patterns in using", () => {
        expect("using {a} = {}").not.toEval();
        expect("using a, {a} = {}").not.toEval();
        expect("using a = null, [b] = [null]").not.toEval();
    });

    test("using with array pattern is valid array access", () => {
        const using = [0, 9999];
        const a = 1;

        expect(eval("using [a] = 1")).toBe(1);
        expect(using[1]).toBe(1);

        expect(eval("using [{a: a}, a] = 2")).toBe(2);
        expect(using[1]).toBe(2);

        expect(eval("using [a, a] = 3")).toBe(3);
        expect(using[1]).toBe(3);

        expect(eval("using [[a, a], a] = 4")).toBe(4);
        expect(using[1]).toBe(4);

        expect(eval("using [2, 1, a] = 5")).toBe(5);
        expect(using[1]).toBe(5);
    });

    test("declaration without initializer", () => {
        expect("using a").not.toEval();
    });

    test("no repeat declarations in single using", () => {
        expect("using a = null, a = null;").not.toEval();
    });

    test("cannot have a using declaration named let", () => {
        expect("using let = null").not.toEval();
    });
});
