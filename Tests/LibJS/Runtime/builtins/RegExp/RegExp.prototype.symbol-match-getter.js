test("an @@match getter is called by split after @@match became non-writable", () => {
    for (let i = 0; i < 10; ++i) expect("a,b".split(/,/)).toEqual(["a", "b"]);
    Object.defineProperty(RegExp.prototype, Symbol.match, { writable: false });
    for (let i = 0; i < 10; ++i) expect("a,b".split(/,/)).toEqual(["a", "b"]);
    let calls = 0;
    Object.defineProperty(RegExp.prototype, Symbol.match, {
        get() {
            ++calls;
            return true;
        },
    });
    expect("a,b".split(/,/)).toEqual(["a", "b"]);
    expect(calls).toBeGreaterThan(0);
});
