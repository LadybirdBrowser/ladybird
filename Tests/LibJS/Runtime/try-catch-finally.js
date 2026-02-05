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

test("try statement must have either 'catch' or 'finally' clause", () => {
    expect("try {} catch {}").toEval();
    expect("try {} catch (e) {}").toEval();
    expect("try {} finally {}").toEval();
    expect("try {} catch {} finally {}").toEval();
    expect("try {} catch (e) {} finally {}").toEval();
    expect("try {}").not.toEval();
});
