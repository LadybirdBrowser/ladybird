test("Issue #1992, shrinking array during find() iteration", () => {
    var a, callbackCalled;

    callbackCalled = 0;
    a = [1, 2, 3, 4, 5];
    a.find(() => {
        callbackCalled++;
        a.pop();
    });
    expect(callbackCalled).toBe(5);

    callbackCalled = 0;
    a = [1, 2, 3, 4, 5];
    a.findIndex(() => {
        callbackCalled++;
        a.pop();
    });
    expect(callbackCalled).toBe(5);
});
