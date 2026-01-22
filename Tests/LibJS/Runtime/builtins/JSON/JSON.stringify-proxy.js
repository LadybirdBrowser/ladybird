test("basic functionality", () => {
    let p = new Proxy([], {
        get(_, key) {
            if (key === "length") return 3;
            return Number(key);
        },
    });

    expect(JSON.stringify(p)).toBe("[0,1,2]");
    expect(JSON.stringify([[new Proxy(p, {})]])).toBe("[[[0,1,2]]]");
});
