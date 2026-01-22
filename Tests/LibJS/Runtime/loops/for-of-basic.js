describe("correct behavior", () => {
    test("iterate through array", () => {
        const a = [];
        for (const num of [1, 2, 3]) {
            a.push(num);
        }
        expect(a).toEqual([1, 2, 3]);
    });

    test("iterate through string", () => {
        const a = [];
        for (const char of "hello") {
            a.push(char);
        }
        expect(a).toEqual(["h", "e", "l", "l", "o"]);
    });

    test("iterate through string object", () => {
        const a = [];
        for (const char of new String("hello")) {
            a.push(char);
        }
        expect(a).toEqual(["h", "e", "l", "l", "o"]);
    });

    test("use already-declared variable", () => {
        var char;
        for (char of "abc");
        expect(char).toBe("c");
    });

    test("respects custom Symbol.iterator method", () => {
        const o = {
            [Symbol.iterator]() {
                return {
                    i: 0,
                    next() {
                        if (this.i++ == 3) {
                            return { done: true };
                        }
                        return { value: this.i, done: false };
                    },
                };
            },
        };

        const a = [];
        for (const k of o) {
            a.push(k);
        }

        expect(a).toEqual([1, 2, 3]);
    });

    test("loops through custom iterator if there is an exception thrown part way through", () => {
        // This tests against the way custom iterators used to be implemented, where the values
        // were all collected at once before the for-of body was executed, instead of getting
        // the values one at a time
        const o = {
            [Symbol.iterator]() {
                return {
                    i: 0,
                    next() {
                        if (this.i++ === 3) {
                            throw new Error();
                        }
                        return { value: this.i };
                    },
                };
            },
        };

        const a = [];

        try {
            for (let k of o) {
                a.push(k);
            }
            expect().fail();
        } catch (e) {
            expect(a).toEqual([1, 2, 3]);
        }
    });
});

describe("errors", () => {
    test("right hand side is a primitive", () => {
        expect(() => {
            for (const _ of 123) {
            }
        }).toThrowWithMessage(TypeError, "123 is not iterable");
    });

    test("right hand side is an object", () => {
        expect(() => {
            for (const _ of { foo: 1, bar: 2 }) {
            }
        }).toThrowWithMessage(TypeError, "[object Object] is not iterable");
    });
});

test("allow binding patterns", () => {
    let counter = 0;
    for (let [a, b] of [
        [1, 2],
        [3, 4],
        [5, 6],
    ]) {
        expect(a + 1).toBe(b);
        counter++;
    }
    expect(counter).toBe(3);
});

describe("special left hand sides", () => {
    test("allow member expression as variable", () => {
        const f = {};
        for (f.a of "abc");
        expect(f.a).toBe("c");
    });

    test("allow member expression of function call", () => {
        const b = {};
        function f() {
            return b;
        }

        for (f().a of "abc");

        expect(f().a).toBe("c");
    });

    test.xfail("call function is allowed in parsing but fails in runtime", () => {
        function f() {
            expect().fail();
        }

        // Does not fail since it does not iterate but prettier does not like it so we use eval.
        expect("for (f() of []);").toEvalTo(undefined);

        expect(() => {
            eval("for (f() of [0]) { expect().fail() }");
        }).toThrowWithMessage(ReferenceError, "Invalid left-hand side in assignment");
    });

    test("Cannot change constant declaration in body", () => {
        const vals = [];
        for (const v of [1, 2]) {
            expect(() => v++).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
            vals.push(v);
        }

        expect(vals).toEqual([1, 2]);
    });
});
