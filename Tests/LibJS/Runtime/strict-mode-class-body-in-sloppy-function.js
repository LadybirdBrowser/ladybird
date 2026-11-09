function assignsInComputedKey() {
    class C {
        [(undeclaredInComputedKey = "a")]() {}
    }
}

function assignsInHeritage() {
    class C extends (undeclaredInHeritage = Object) {}
}

function assignsAfterClass() {
    class C {
        m() {}
    }
    undeclaredAfterClass = 1;
    return undeclaredAfterClass;
}

function deletesBeforeAndAfterClass() {
    const object = Object.freeze({ a: 1 });
    const before = delete object.a;
    class C {}
    const after = delete object.a;
    return [before, after];
}

test("assignments in a class heritage or computed key are strict", () => {
    for (let i = 0; i < 3; ++i) {
        expect(assignsInComputedKey).toThrowWithMessage(ReferenceError, "'undeclaredInComputedKey' is not defined");
        expect(assignsInHeritage).toThrowWithMessage(ReferenceError, "'undeclaredInHeritage' is not defined");
    }
});

test("code around a class in a sloppy function stays sloppy", () => {
    for (let i = 0; i < 3; ++i) {
        expect(assignsAfterClass()).toBe(1);
        expect(deletesBeforeAndAfterClass()).toEqual([false, false]);
    }
});
