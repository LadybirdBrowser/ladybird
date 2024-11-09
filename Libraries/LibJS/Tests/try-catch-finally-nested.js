test("Nested try/catch/finally with exceptions", () => {
    // This test uses a combination of boolean "checkpoint" flags
    // and expect().fail() to ensure certain code paths have been
    // reached and others haven't.
    var level1TryHasBeenExecuted = false;
    var level1CatchHasBeenExecuted = false;
    var level1FinallyHasBeenExecuted = false;
    var level2TryHasBeenExecuted = false;
    var level2CatchHasBeenExecuted = false;
    var level3TryHasBeenExecuted = false;
    var level3CatchHasBeenExecuted = false;
    var level3FinallyHasBeenExecuted = false;
    expect(() => {
        try {
            level1TryHasBeenExecuted = true;
            foo();
            expect().fail();
        } catch (e) {
            level1CatchHasBeenExecuted = true;
            try {
                level2TryHasBeenExecuted = true;
                try {
                    level3TryHasBeenExecuted = true;
                    bar();
                    expect().fail();
                } catch (e) {
                    level3CatchHasBeenExecuted = true;
                } finally {
                    level3FinallyHasBeenExecuted = true;
                    baz();
                    expect().fail();
                }
                expect().fail();
            } catch (e) {
                level2CatchHasBeenExecuted = true;
                qux();
                expect().fail();
            }
            expect().fail();
        } finally {
            level1FinallyHasBeenExecuted = true;
            throw Error("Error in final finally");
            expect().fail();
        }
        expect().fail();
    }).toThrow(Error, "Error in final finally");
    expect(level1TryHasBeenExecuted).toBeTrue();
    expect(level1CatchHasBeenExecuted).toBeTrue();
    expect(level1FinallyHasBeenExecuted).toBeTrue();
    expect(level2TryHasBeenExecuted).toBeTrue();
    expect(level2CatchHasBeenExecuted).toBeTrue();
    expect(level3TryHasBeenExecuted).toBeTrue();
    expect(level3CatchHasBeenExecuted).toBeTrue();
    expect(level3FinallyHasBeenExecuted).toBeTrue();
});

test("Nested try/catch/finally with return in inner context", () => {
    success = false;
    (() => {
        try {
            try {
                return;
            } catch (e) {
                expect().fail();
            }
        } finally {
            success = true;
        }
        expect().fail();
    })();
    expect(success).toBeTrue();
});

test("Deeply nested try/catch/finally with return in inner context", () => {
    success = 0;
    (() => {
        try {
            try {
                try {
                    try {
                        try {
                            return;
                        } catch (e) {
                            expect().fail();
                        } finally {
                            success += 4;
                        }
                    } catch (e) {
                        expect().fail();
                    }
                } catch (e) {
                    expect().fail();
                } finally {
                    success += 2;
                }
            } catch (e) {
                expect().fail();
            }
        } finally {
            success += 1;
        }
        expect().fail();
    })();
    expect(success).toBe(7);
});

test("Nested try/finally/catch with exception in inner context ", () => {
    success = 0;
    try {
        try {
            throw Error("Error in inner try");
        } finally {
            success += 1;
        }
        expect.fail();
    } catch (e) {
        success += 1;
    }
    expect(success).toBe(2);
});

test("Nested try/catch/finally with exception in inner most finally inside loop", () => {
    success = 0;
    try {
        try {
            do {
                try {
                    throw 1;
                } finally {
                    break;
                }
                expect.fail();
            } while (expect.fail());
        } catch (e) {
            expect.fail();
        }
    } finally {
        success = 1;
    }
    expect(success).toBe(1);
});
