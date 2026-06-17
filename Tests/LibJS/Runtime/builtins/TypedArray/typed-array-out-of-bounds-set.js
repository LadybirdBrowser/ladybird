test("out-of-bounds element write still evaluates ToNumber for its side effects", () => {
    function makeValue(counter) {
        return {
            valueOf() {
                counter.count++;
                return 1;
            },
        };
    }

    // Direct assignment: ToNumber runs even though the store is discarded.
    let counter = { count: 0 };
    let ta = new Int32Array(0);
    for (let i = 0; i < 5; ++i) ta[0] = makeValue(counter);
    expect(counter.count).toBe(5);

    // Reflect.set: same observable behavior.
    counter = { count: 0 };
    ta = new Int32Array(0);
    for (let i = 0; i < 5; ++i) expect(Reflect.set(ta, 0, makeValue(counter))).toBeTrue();
    expect(counter.count).toBe(5);

    // Reflect.defineProperty: does not evaluate ToNumber for an invalid index.
    counter = { count: 0 };
    ta = new Int32Array(0);
    for (let i = 0; i < 5; ++i) {
        expect(
            Reflect.defineProperty(ta, 0, {
                value: makeValue(counter),
                writable: true,
                enumerable: true,
                configurable: true,
            })
        ).toBeFalse();
    }
    expect(counter.count).toBe(0);
});
