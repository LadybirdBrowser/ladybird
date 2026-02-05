test("Issue #3437, exception thrown in catch {} block", () => {
    var tryHasBeenExecuted = false;
    var catchHasBeenExecuted = false;
    var finallyHasBeenExecuted = false;
    expect(() => {
        try {
            tryHasBeenExecuted = true;
            foo();
            // execution must not reach this step
            expect().fail();
        } catch (e) {
            catchHasBeenExecuted = true;
            bar();
            // ...also not this step
            expect().fail();
        } finally {
            finallyHasBeenExecuted = true;
        }
        // ...or this step
        expect().fail();
    }).toThrow(ReferenceError, "'bar' is not defined");
    expect(tryHasBeenExecuted).toBeTrue();
    expect(catchHasBeenExecuted).toBeTrue();
    expect(finallyHasBeenExecuted).toBeTrue();
});
