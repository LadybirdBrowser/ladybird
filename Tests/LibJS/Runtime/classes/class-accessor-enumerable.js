test("class accessor should be non-enumerable", () => {
    class C {
        get x() {
            return 1;
        }
        set x(v) {}
    }

    const desc = Object.getOwnPropertyDescriptor(C.prototype, "x");
    expect(desc.enumerable).toBeFalse();
    expect(Object.keys(C.prototype).includes("x")).toBeFalse();
});
