describe("basic usage", () => {
    test("disposes after block exit", () => {
        let disposed = false;
        let inBlock = false;
        {
            expect(disposed).toBeFalse();
            using a = {
                [Symbol.dispose]() {
                    disposed = true;
                },
            };
            inBlock = true;
            expect(disposed).toBeFalse();
        }

        expect(inBlock).toBeTrue();
        expect(disposed).toBeTrue();
    });

    test("disposes in reverse order after block exit", () => {
        const disposed = [];
        {
            expect(disposed).toHaveLength(0);
            using a = {
                [Symbol.dispose]() {
                    disposed.push("a");
                },
            };
            using b = {
                [Symbol.dispose]() {
                    disposed.push("b");
                },
            };
            expect(disposed).toHaveLength(0);
        }

        expect(disposed).toEqual(["b", "a"]);
    });

    test("disposes in reverse order after block exit even in same declaration", () => {
        const disposed = [];
        {
            expect(disposed).toHaveLength(0);
            using a = {
                    [Symbol.dispose]() {
                        disposed.push("a");
                    },
                },
                b = {
                    [Symbol.dispose]() {
                        disposed.push("b");
                    },
                };
            expect(disposed).toHaveLength(0);
        }

        expect(disposed).toEqual(["b", "a"]);
    });
});

describe("behavior with exceptions", () => {
    function ExpectedError(name) {
        this.name = name;
    }

    test("is run even after throw", () => {
        let disposed = false;
        let inBlock = false;
        let inCatch = false;
        try {
            expect(disposed).toBeFalse();
            using a = {
                [Symbol.dispose]() {
                    disposed = true;
                },
            };
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

    test("throws error if dispose method does", () => {
        let disposed = false;
        let endOfTry = false;
        let inCatch = false;
        try {
            expect(disposed).toBeFalse();
            using a = {
                [Symbol.dispose]() {
                    disposed = true;
                    throw new ExpectedError();
                },
            };
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

    test("if block and using throw get suppressed error", () => {
        let disposed = false;
        let inCatch = false;
        try {
            expect(disposed).toBeFalse();
            using a = {
                [Symbol.dispose]() {
                    disposed = true;
                    throw new ExpectedError("dispose");
                },
            };
            expect(disposed).toBeFalse();
            throw new ExpectedError("throw");
        } catch (e) {
            expect(disposed).toBeTrue();
            expect(e).toBeInstanceOf(SuppressedError);
            expect(e.error).toBeInstanceOf(ExpectedError);
            expect(e.error.name).toBe("dispose");
            expect(e.suppressed).toBeInstanceOf(ExpectedError);
            expect(e.suppressed.name).toBe("throw");
            inCatch = true;
        }
        expect(disposed).toBeTrue();
        expect(inCatch).toBeTrue();
    });

    test("multiple throwing disposes give suppressed error", () => {
        let inCatch = false;
        try {
            {
                using a = {
                    [Symbol.dispose]() {
                        throw new ExpectedError("a");
                    },
                };

                using b = {
                    [Symbol.dispose]() {
                        throw new ExpectedError("b");
                    },
                };
            }

            expect().fail();
        } catch (e) {
            expect(e).toBeInstanceOf(SuppressedError);
            expect(e.error).toBeInstanceOf(ExpectedError);
            expect(e.error.name).toBe("a");
            expect(e.suppressed).toBeInstanceOf(ExpectedError);
            expect(e.suppressed.name).toBe("b");
            inCatch = true;
        }
        expect(inCatch).toBeTrue();
    });

    test("3 throwing disposes give chaining suppressed error", () => {
        let inCatch = false;
        try {
            {
                using a = {
                    [Symbol.dispose]() {
                        throw new ExpectedError("a");
                    },
                };

                using b = {
                    [Symbol.dispose]() {
                        throw new ExpectedError("b");
                    },
                };

                using c = {
                    [Symbol.dispose]() {
                        throw new ExpectedError("c");
                    },
                };
            }

            expect().fail();
        } catch (e) {
            expect(e).toBeInstanceOf(SuppressedError);
            expect(e.error).toBeInstanceOf(ExpectedError);
            expect(e.error.name).toBe("a");
            expect(e.suppressed).toBeInstanceOf(SuppressedError);

            const inner = e.suppressed;

            expect(inner.error).toBeInstanceOf(ExpectedError);
            expect(inner.error.name).toBe("b");
            expect(inner.suppressed).toBeInstanceOf(ExpectedError);
            expect(inner.suppressed.name).toBe("c");
            inCatch = true;
        }
        expect(inCatch).toBeTrue();
    });

    test("normal error and multiple disposing errors give chaining suppressed errors", () => {
        let inCatch = false;
        try {
            using a = {
                [Symbol.dispose]() {
                    throw new ExpectedError("a");
                },
            };

            using b = {
                [Symbol.dispose]() {
                    throw new ExpectedError("b");
                },
            };

            throw new ExpectedError("top");
        } catch (e) {
            expect(e).toBeInstanceOf(SuppressedError);
            expect(e.error).toBeInstanceOf(ExpectedError);
            expect(e.error.name).toBe("a");
            expect(e.suppressed).toBeInstanceOf(SuppressedError);

            const inner = e.suppressed;

            expect(inner.error).toBeInstanceOf(ExpectedError);
            expect(inner.error.name).toBe("b");
            expect(inner.suppressed).toBeInstanceOf(ExpectedError);
            expect(inner.suppressed.name).toBe("top");
            inCatch = true;
        }
        expect(inCatch).toBeTrue();
    });
});

describe("works in a bunch of scopes", () => {
    test("works in block", () => {
        let dispose = false;
        expect(dispose).toBeFalse();
        {
            expect(dispose).toBeFalse();
            using a = {
                [Symbol.dispose]() {
                    dispose = true;
                },
            };
            expect(dispose).toBeFalse();
        }
        expect(dispose).toBeTrue();
    });

    test("works in static class block", () => {
        let dispose = false;
        expect(dispose).toBeFalse();
        class A {
            static {
                expect(dispose).toBeFalse();
                using a = {
                    [Symbol.dispose]() {
                        dispose = true;
                    },
                };
                expect(dispose).toBeFalse();
            }
        }
        expect(dispose).toBeTrue();
    });

    test("works in function", () => {
        let dispose = [];
        function f(val) {
            const disposeLength = dispose.length;
            using a = {
                [Symbol.dispose]() {
                    dispose.push(val);
                },
            };
            expect(dispose.length).toBe(disposeLength);
        }
        expect(dispose).toEqual([]);
        f(0);
        expect(dispose).toEqual([0]);
        f(1);
        expect(dispose).toEqual([0, 1]);
    });

    test("blocks in switch cases dispose at their end", () => {
        let disposeFull = [];
        let disposeInner = false;

        function pusher(val) {
            return {
                val,
                [Symbol.dispose]() {
                    disposeFull.push(val);
                },
            };
        }

        switch (2) {
            case 3: {
                using notDisposed = {
                    [Symbol.dispose]() {
                        expect().fail("not-disposed 1");
                    },
                };
            }
            case 2: {
                expect(disposeFull).toEqual([]);
                using a = pusher("a");
                expect(disposeFull).toEqual([]);

                using b = pusher("b");
                expect(disposeFull).toEqual([]);
                expect(b.val).toBe("b");

                expect(disposeInner).toBeFalse();
            }
            // fallthrough
            case 1:
                {
                    expect(disposeFull).toEqual(["b", "a"]);
                    expect(disposeInner).toBeFalse();

                    using inner = {
                        [Symbol.dispose]() {
                            disposeInner = true;
                        },
                    };

                    expect(disposeInner).toBeFalse();
                }
                expect(disposeInner).toBeTrue();
                {
                    using c = pusher("c");
                    expect(c.val).toBe("c");
                }
                break;
            case 0: {
                using notDisposed2 = {
                    [Symbol.dispose]() {
                        expect().fail("not-disposed 2");
                    },
                };
            }
        }

        expect(disposeInner).toBeTrue();
        expect(disposeFull).toEqual(["b", "a", "c"]);
    });
});

describe("invalid using bindings", () => {
    test("nullish values do not throw", () => {
        using a = null,
            b = undefined;
        expect(a).toBeNull();
        expect(b).toBeUndefined();
    });

    test("non-object throws", () => {
        [0, "a", true, NaN, 4n, Symbol.dispose].forEach(value => {
            expect(() => {
                using v = value;
            }).toThrowWithMessage(TypeError, "is not an object");
        });
    });

    test("object without dispose throws", () => {
        expect(() => {
            using a = {};
        }).toThrowWithMessage(TypeError, "does not have dispose method");
    });

    test("object with non callable dispose throws", () => {
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

    test("using", () => {
        "use strict";
        using using = null;
        expect(using).toBeNull();
    });

    test("function", () => {
        "use strict";
        function using() {
            return 1;
        }
        expect(using()).toBe(1);
    });
});

describe("syntax errors / weird artifacts which remain valid", () => {
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

describe("control flow", () => {
    const disposable = (log, name) => ({
        [Symbol.dispose]() {
            log.push(name);
        },
    });

    test("break and continue to labels dispose of every scope they leave", () => {
        const log = [];
        outer: for (let i = 0; i < 2; i++) {
            using a = disposable(log, `a${i}`);
            for (let j = 0; j < 2; j++) {
                using b = disposable(log, `b${i}${j}`);
                {
                    using c = disposable(log, `c${i}${j}`);
                    if (i === 0) continue outer;
                    break outer;
                }
            }
        }
        expect(log).toEqual(["c00", "b00", "a0", "c10", "b10", "a1"]);

        log.length = 0;
        block: {
            using a = disposable(log, "a");
            {
                using b = disposable(log, "b");
                break block;
            }
        }
        expect(log).toEqual(["b", "a"]);
    });

    test("return from a finally block disposes of every scope it leaves", () => {
        const log = [];
        function f() {
            using a = disposable(log, "a");
            try {
                using b = disposable(log, "b");
                return 1;
            } finally {
                using c = disposable(log, "c");
                return 2;
            }
        }
        expect(f()).toBe(2);
        expect(log).toEqual(["b", "c", "a"]);
    });

    test("completion value of a block", () => {
        const log = [];
        expect(eval("{ using a = disposable(log, 'a'); 42; }")).toBe(42);
        expect(eval("1; { using b = disposable(log, 'b'); }")).toBe(1);
        expect(eval("l: { using c = disposable(log, 'c'); 3; break l; }")).toBe(3);
        expect(log).toEqual(["a", "b", "c"]);
    });

    test("generator return() and throw() while suspended in a scope", () => {
        const log = [];
        function* generator() {
            using a = disposable(log, "a");
            try {
                using b = disposable(log, "b");
                yield 1;
                log.push("resumed");
            } finally {
                log.push("finally");
            }
        }

        let iterator = generator();
        iterator.next();
        expect(iterator.return(5)).toEqual({ value: 5, done: true });
        expect(log).toEqual(["b", "finally", "a"]);

        log.length = 0;
        iterator = generator();
        iterator.next();
        expect(() => iterator.throw(new Error("thrown"))).toThrowWithMessage(Error, "thrown");
        expect(log).toEqual(["b", "finally", "a"]);

        log.length = 0;
        iterator = generator();
        iterator.next();
        gc();
        expect(iterator.next()).toEqual({ value: undefined, done: true });
        expect(log).toEqual(["resumed", "b", "finally", "a"]);
    });

    test("for-of disposes of the resource before closing the iterator", () => {
        const log = [];
        function makeIterator(values) {
            let index = 0;
            return {
                [Symbol.iterator]() {
                    return this;
                },
                next() {
                    return index < values.length ? { value: values[index++], done: false } : { done: true };
                },
                return() {
                    log.push("return");
                    return {};
                },
            };
        }

        const throwing = {
            [Symbol.dispose]() {
                log.push("dispose");
                throw new Error("dispose");
            },
        };
        expect(() => {
            for (using x of makeIterator([throwing])) break;
        }).toThrowWithMessage(Error, "dispose");
        expect(log).toEqual(["dispose", "return"]);

        log.length = 0;
        expect(() => {
            for (using x of makeIterator([1])) log.push("body");
        }).toThrow(TypeError);
        expect(log).toEqual(["return"]);

        log.length = 0;
        function* generator() {
            for (using x of makeIterator([disposable(log, "x")])) yield 1;
        }
        const iterator = generator();
        iterator.next();
        iterator.return();
        expect(log).toEqual(["x", "return"]);
    });
});

describe("parsing", () => {
    test("using followed by a reserved word is an expression", () => {
        expect("{ var using, x = {}; using in x; using instanceof Object; }").toEval();
    });

    test("using declarations are allowed in the body of a dynamic function", () => {
        const log = [];
        const f = new Function("log", "using a = { [Symbol.dispose]() { log.push('a'); } }; log.push('body');");
        f(log);
        expect(log).toEqual(["body", "a"]);
    });
});
