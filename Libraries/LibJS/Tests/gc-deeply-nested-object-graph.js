test("garbage collection of a deeply-nested object graph", () => {
    let root = {};
    let o = root;

    for (let i = 0; i < 200_000; ++i) {
        o.next = {};
        o = o.next;
    }

    gc();
});
