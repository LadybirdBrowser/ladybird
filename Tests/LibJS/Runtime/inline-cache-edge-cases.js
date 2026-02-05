test("Inline cache invalidated by deleting property from unique shape", () => {
    // Create an object with an unique shape by adding a huge amount of properties.
    let o = {};
    for (let x = 0; x < 1000; ++x) {
        o["prop" + x] = x;
    }

    function ic(o) {
        return o.prop2;
    }

    let first = ic(o);
    delete o.prop2;
    let second = ic(o);

    expect(first).toBe(2);
    expect(second).toBeUndefined();
});
