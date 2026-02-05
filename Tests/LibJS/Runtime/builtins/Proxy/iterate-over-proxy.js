test("iterate over bogus proxy", () => {
    expect(() => {
        let proxy = new Proxy([123], {
            getOwnPropertyDescriptor: function (p) {
                return undefined;
            },
        });

        for (const p in proxy) {
        }
    }).toThrow();
});
