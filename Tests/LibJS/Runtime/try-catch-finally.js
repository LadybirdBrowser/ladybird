test("try/catch without exception", () => {
    var tryHasBeenExecuted = false;
    var catchHasBeenExecuted = false;
    try {
        tryHasBeenExecuted = true;
    } catch (e) {
        catchHasBeenExecuted = true;
    }
    expect(tryHasBeenExecuted).toBeTrue();
    expect(catchHasBeenExecuted).toBeFalse();
});

test("try/catch with exception in try", () => {
    var tryHasBeenExecuted = false;
    var catchHasBeenExecuted = false;
    var tryError = Error("Error in try");
    try {
        tryHasBeenExecuted = true;
        throw tryError;
        expect().fail();
    } catch (e) {
        catchHasBeenExecuted = true;
        expect(e).toBe(tryError);
    }
    expect(tryHasBeenExecuted).toBeTrue();
    expect(catchHasBeenExecuted).toBeTrue();
});

test("try/catch with exception in try and catch", () => {
    var tryHasBeenExecuted = false;
    var catchHasBeenExecuted = false;
    var tryError = Error("Error in try");
    var catchError = Error("Error in catch");
    expect(() => {
        try {
            tryHasBeenExecuted = true;
            throw tryError;
            expect().fail();
        } catch (e) {
            catchHasBeenExecuted = true;
            expect(e).toBe(tryError);
            throw catchError;
            expect().fail();
        }
    }).toThrow(Error, "Error in catch");
    expect(tryHasBeenExecuted).toBeTrue();
    expect(catchHasBeenExecuted).toBeTrue();
});

test("try/catch/finally without exception", () => {
    var tryHasBeenExecuted = false;
    var catchHasBeenExecuted = false;
    var finallyHasBeenExecuted = false;
    try {
        tryHasBeenExecuted = true;
    } catch (e) {
        catchHasBeenExecuted = true;
    } finally {
        finallyHasBeenExecuted = true;
    }
    expect(tryHasBeenExecuted).toBeTrue();
    expect(catchHasBeenExecuted).toBeFalse();
    expect(finallyHasBeenExecuted).toBeTrue();
});

test("try/catch/finally with exception in try and catch", () => {
    var tryHasBeenExecuted = false;
    var catchHasBeenExecuted = false;
    var finallyHasBeenExecuted = false;
    var tryError = Error("Error in try");
    var catchError = Error("Error in catch");
    expect(() => {
        try {
            tryHasBeenExecuted = true;
            throw tryError;
            expect().fail();
        } catch (e) {
            catchHasBeenExecuted = true;
            expect(e).toBe(tryError);
            throw catchError;
            expect().fail();
        } finally {
            finallyHasBeenExecuted = true;
        }
    }).toThrow(Error, "Error in catch");
    expect(tryHasBeenExecuted).toBeTrue();
    expect(catchHasBeenExecuted).toBeTrue();
    expect(finallyHasBeenExecuted).toBeTrue();
});

test("try/catch/finally with exception in finally", () => {
    var tryHasBeenExecuted = false;
    var catchHasBeenExecuted = false;
    var finallyHasBeenExecuted = false;
    var finallyError = Error("Error in finally");
    expect(() => {
        try {
            tryHasBeenExecuted = true;
        } catch (e) {
            catchHasBeenExecuted = true;
        } finally {
            finallyHasBeenExecuted = true;
            throw finallyError;
            expect().fail();
        }
    }).toThrow(Error, "Error in finally");
    expect(tryHasBeenExecuted).toBeTrue();
    expect(catchHasBeenExecuted).toBeFalse();
    expect(finallyHasBeenExecuted).toBeTrue();
});

test("try/catch/finally with exception in try and finally", () => {
    var tryHasBeenExecuted = false;
    var catchHasBeenExecuted = false;
    var finallyHasBeenExecuted = false;
    var tryError = Error("Error in try");
    var finallyError = Error("Error in finally");
    expect(() => {
        try {
            tryHasBeenExecuted = true;
            throw tryError;
            expect().fail();
        } catch (e) {
            catchHasBeenExecuted = true;
            expect(e).toBe(tryError);
        } finally {
            finallyHasBeenExecuted = true;
            throw finallyError;
            expect().fail();
        }
    }).toThrow(Error, "Error in finally");
    expect(tryHasBeenExecuted).toBeTrue();
    expect(catchHasBeenExecuted).toBeTrue();
    expect(finallyHasBeenExecuted).toBeTrue();
});

test("try/catch/finally with exception in try, catch and finally", () => {
    var tryHasBeenExecuted = false;
    var catchHasBeenExecuted = false;
    var finallyHasBeenExecuted = false;
    var tryError = Error("Error in try");
    var catchError = Error("Error in catch");
    var finallyError = Error("Error in finally");
    expect(() => {
        try {
            tryHasBeenExecuted = true;
            throw tryError;
            expect().fail();
        } catch (e) {
            catchHasBeenExecuted = true;
            expect(e).toBe(tryError);
            throw catchError;
            expect().fail();
        } finally {
            finallyHasBeenExecuted = true;
            throw finallyError;
            expect().fail();
        }
    }).toThrow(Error, "Error in finally");
    expect(tryHasBeenExecuted).toBeTrue();
    expect(catchHasBeenExecuted).toBeTrue();
    expect(finallyHasBeenExecuted).toBeTrue();
});

test("exception from a variable read in a callee is caught by the caller", () => {
    function call(read) {
        try {
            read();
        } catch (error) {
            return error;
        }
        return null;
    }
    for (let padding = 0; padding < 64; ++padding) {
        const read = new Function(`return () => { ${"debugger; ".repeat(padding)}return x; }; let x;`)();
        expect(call(read)).toBeInstanceOf(ReferenceError);
        expect(call(read)).toBeInstanceOf(ReferenceError);
    }
});

test("try statement must have either 'catch' or 'finally' clause", () => {
    expect("try {} catch {}").toEval();
    expect("try {} catch (e) {}").toEval();
    expect("try {} finally {}").toEval();
    expect("try {} catch {} finally {}").toEval();
    expect("try {} catch (e) {} finally {}").toEval();
    expect("try {}").not.toEval();
});

test("lexical declarations in a catch block may not redeclare the catch parameter", () => {
    expect("try {} catch (e) { let { e } = {}; }").not.toEval();
    expect("try {} catch ({ e }) { const [e] = []; }").not.toEval();
    expect("try {} catch (e) { using e = null; }").not.toEval();
});

test("var declaration named like the catch parameter leaves the function binding undefined", () => {
    function local() {
        try {
            throw 1;
        } catch (e) {
            var e = 2;
        }
        return typeof e;
    }
    expect(local()).toBe("undefined");

    function captured() {
        try {
            throw 1;
        } catch (e) {
            var e = 2;
        }
        return (() => e)();
    }
    expect(captured()).toBeUndefined();
});

describe("names of a catch parameter's binding pattern are uninitialized until they are bound", () => {
    function caughtName(thrown, handler) {
        try {
            handler(thrown);
        } catch (error) {
            return error.name;
        }
        return "nothing";
    }

    test("read by an initializer", () => {
        expect(
            caughtName({}, value => {
                try {
                    throw value;
                } catch ({ a = b, b }) {}
            })
        ).toBe("ReferenceError");
        expect(
            caughtName([], value => {
                try {
                    throw value;
                } catch ([a = b, b]) {}
            })
        ).toBe("ReferenceError");
    });

    test("read by an initializer on each iteration of a loop", () => {
        const results = [];
        for (let i = 0; i < 2; i++) {
            try {
                try {
                    throw { b: i === 0 ? undefined : 1 };
                } catch ({ a = i === 0 ? 0 : b, b }) {
                    results.push(a);
                }
            } catch (error) {
                results.push(error.name);
            }
        }
        expect(results).toEqual([0, "ReferenceError"]);
    });

    test("read once they are bound", () => {
        try {
            throw { a: 1 };
        } catch ({ a, b = a }) {
            expect(b).toBe(1);
        }
    });
});
