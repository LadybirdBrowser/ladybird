test("proxy traps should be invoked in the correct order", () => {
    var log = [];
    var target = [];
    var proxy = new Proxy(
        target,
        new Proxy(
            {},
            {
                get(t, pk, r) {
                    log.push(pk);
                },
            }
        )
    );
    proxy.push(1);

    expect(log.length, 8);
    expect(log[0]).toBe("get");
    expect(log[1]).toBe("get");
    expect(log[2]).toBe("set");
    expect(log[3]).toBe("getOwnPropertyDescriptor");
    expect(log[4]).toBe("defineProperty");
    expect(log[5]).toBe("set");
    expect(log[6]).toBe("getOwnPropertyDescriptor");
    expect(log[7]).toBe("defineProperty");
});
