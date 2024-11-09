describe("basic usage", () => {
    test.xfail("using in normal for loop", () => {
        let isDisposed = false;
        let lastI = -1;
        for (
            using x = {
                i: 0,
                tick() {
                    this.i++;
                },
                done() {
                    return this.i === 3;
                },
                [Symbol.dispose]() {
                    isDisposed = true;
                },
            };
            !x.done();
            x.tick()
        ) {
            expect(isDisposed).toBeFalse();
            expect(x.i).toBeGreaterThan(lastI);
            lastI = x.i;
        }

        expect(isDisposed).toBeTrue();
        expect(lastI).toBe(2);
    });

    test.xfail("using in normal for loop with expression body", () => {
        let isDisposed = false;
        let outerI = 0;
        for (
            using x = {
                i: 0,
                tick() {
                    this.i++;
                    outerI++;
                },
                done() {
                    return this.i === 3;
                },
                [Symbol.dispose]() {
                    isDisposed = true;
                },
            };
            !x.done();
            x.tick()
        )
            expect(isDisposed).toBeFalse();

        expect(isDisposed).toBeTrue();
        expect(outerI).toBe(3);
    });

    test.xfail("using in for of loop", () => {
        const disposable = [];
        const values = [];

        function createDisposable(value) {
            return {
                value: value,
                [Symbol.dispose]() {
                    expect(this.value).toBe(value);
                    disposable.push(value);
                }
            };
        }

        for (using a of [createDisposable('a'), createDisposable('b'), createDisposable('c')]) {
            expect(disposable).toEqual(values);
            values.push(a.value);
        }

        expect(disposable).toEqual(['a', 'b', 'c']);
    });

    test.xfail("using in for of loop with expression body", () => {
        let disposableCalls = 0;
        let i = 0;

        const obj = {
            [Symbol.dispose]() {
                disposableCalls++;
            }
        };

        for (using a of [obj, obj, obj])
            expect(disposableCalls).toBe(i++);

        expect(disposableCalls).toBe(3);
    });

    test.xfail("can have multiple declaration in normal for loop", () => {
        let disposed = 0;
        const a = {
            [Symbol.dispose]() {
                disposed++;
            }
        }

        expect(disposed).toBe(0);
        for (using b = a, c = a; false;)
            expect().fail();

        expect(disposed).toBe(2);
    });

    test.xfail("can have using in block in for loop", () => {
        const disposed = [];
        const values = [];
        for (let i = 0; i < 3; i++) {
            using a = {
                val: i,
                [Symbol.dispose]() {
                    expect(i).toBe(this.val);
                    disposed.push(i);
                },
            };
            expect(disposed).toEqual(values);
            values.push(i);
        }
        expect(disposed).toEqual([0, 1, 2]);
    });

    test.xfail("can have using in block in for-in loop", () => {
        const disposed = [];
        const values = [];
        for (const i in ['a', 'b', 'c']) {
            using a = {
                val: i,
                [Symbol.dispose]() {
                    expect(i).toBe(this.val);
                    disposed.push(i);
                },
            };
            expect(disposed).toEqual(values);
            values.push(i);
        }
        expect(disposed).toEqual(["0", "1", "2"]);
    });

    test.xfail("dispose is called even if throw in for of loop", () => {
        let disposableCalls = 0;

        const obj = {
            [Symbol.dispose]() {
                expect()
                disposableCalls++;
            }
        };

        try {
            for (using a of [obj])
                throw new ExpectationError("Expected in for-of");

            expect().fail("Should have thrown");
        } catch (e) {
            expect(e).toBeInstanceOf(ExpectationError);
            expect(e.message).toBe("Expected in for-of");
            expect(disposableCalls).toBe(1);
        }

        expect(disposableCalls).toBe(1);
    });
});

describe("using is still a valid variable in loops", () => {
    test("for loops var", () => {
        let enteredLoop = false;
        for (var using = 1; using < 2; using++) {
            enteredLoop = true;
        }
        expect(enteredLoop).toBeTrue();
    });

    test("for loops const", () => {
        let enteredLoop = false;
        for (const using = 1; using < 2; ) {
            enteredLoop = true;
            break;
        }
        expect(enteredLoop).toBeTrue();
    });

    test("for loops let", () => {
        let enteredLoop = false;
        for (let using = 1; using < 2; using++) {
            enteredLoop = true;
        }
        expect(enteredLoop).toBeTrue();
    });

    test("using in", () => {
        let enteredLoop = false;
        for (using in [1]) {
            enteredLoop = true;
            expect(using).toBe("0");
        }
        expect(enteredLoop).toBeTrue();
    });

    test("using of", () => {
        let enteredLoop = false;
        for (using of [1]) {
            enteredLoop = true;
            expect(using).toBe(1);
        }
        expect(enteredLoop).toBeTrue();
    });

    test.xfail("using using of", () => {
        let enteredLoop = false;
        for (using using of [null]) {
            enteredLoop = true;
            expect(using).toBeNull();
        }
        expect(enteredLoop).toBeTrue();
    });
});

describe("syntax errors", () => {
    test("cannot have using as for loop body", () => {
        expect("for (;;) using a = {};").not.toEval();
        expect("for (x in []) using a = {};").not.toEval();
        expect("for (x of []) using a = {};").not.toEval();
    });

    test("must have one declaration without initializer in for loop", () => {
        expect("for (using x = {} of []) {}").not.toEval();
        expect("for (using x, y of []) {}").not.toEval();
    });

    test("cannot have using in for-in loop", () => {
        expect("for (using x in []) {}").not.toEval();
        expect("for (using of in []) {}").not.toEval();
        expect("for (using in of []) {}").not.toEval();
    });
});
