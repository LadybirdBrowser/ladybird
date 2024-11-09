test("basic functionality", () => {
    let o = {
        key1: "key1",
        key2: "key2",
        key3: "key3",
    };

    Object.defineProperty(o, "defined", {
        enumerable: true,
        get() {
            o.prop = "prop";
            return "defined";
        },
    });

    o.key4 = "key4";

    o[2] = 2;
    o[0] = 0;
    o[1] = 1;

    delete o.key1;
    delete o.key3;

    o.key1 = "key1";

    expect(JSON.stringify(o)).toBe(
        '{"0":0,"1":1,"2":2,"key2":"key2","defined":"defined","key4":"key4","key1":"key1"}'
    );
});
