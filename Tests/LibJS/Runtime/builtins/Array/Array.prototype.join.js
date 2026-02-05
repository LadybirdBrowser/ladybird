test("length is 1", () => {
    expect(Array.prototype.join).toHaveLength(1);
});

test("basic functionality", () => {
    expect(["hello", "friends"].join()).toBe("hello,friends");
    expect(["hello", "friends"].join(undefined)).toBe("hello,friends");
    expect(["hello", "friends"].join(" ")).toBe("hello friends");
    expect(["hello", "friends", "foo"].join("~", "#")).toBe("hello~friends~foo");
    expect([].join()).toBe("");
    expect([null].join()).toBe("");
    expect([undefined].join()).toBe("");
    expect([undefined, null, ""].join()).toBe(",,");
    expect([1, null, 2, undefined, 3].join()).toBe("1,,2,,3");
    expect(Array(3).join()).toBe(",,");
});

test("circular references", () => {
    const a = ["foo", [], [1, 2, []], ["bar"]];
    a[1] = a;
    a[2][2] = a;
    // [ "foo", <circular>, [ 1, 2, <circular> ], [ "bar" ] ]
    expect(a.join()).toBe("foo,,1,2,,bar");
});
