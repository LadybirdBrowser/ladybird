test("for..in iteration Proxy traps", () => {
    let traps = [];
    let array = [1, 2, 3];
    let from = new Proxy(array, {
        getPrototypeOf: function (t) {
            traps.push("getPrototypeOf");
            return Reflect.getPrototypeOf(t);
        },
        ownKeys: function (t) {
            traps.push("ownKeys");
            return Reflect.ownKeys(t);
        },
        has: function (t, p) {
            traps.push("has");
            return Reflect.has(t, p);
        },
        getOwnPropertyDescriptor: function (t, p) {
            traps.push("getOwnPropertyDescriptor");
            return Reflect.getOwnPropertyDescriptor(t, p);
        },
    });
    const to = [];
    for (const prop in from) {
        to.push(prop);
        from.pop();
    }

    expect(to).toEqual(["0", "1"]);

    expect(traps).toEqual([
        "ownKeys",
        "getPrototypeOf",
        "getOwnPropertyDescriptor",
        "getOwnPropertyDescriptor",
        "getOwnPropertyDescriptor",
        "getOwnPropertyDescriptor",
        "getOwnPropertyDescriptor",
        "getOwnPropertyDescriptor",
    ]);
});
