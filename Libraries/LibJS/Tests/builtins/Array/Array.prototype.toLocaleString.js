test("length is 0", () => {
    expect(Array.prototype.toLocaleString).toHaveLength(0);
});

describe("normal behavior", () => {
    test("array with no elements", () => {
        expect([].toLocaleString()).toBe("");
    });

    test("array with one element", () => {
        expect(["foo"].toLocaleString()).toBe("foo");
    });

    test("array with multiple elements", () => {
        expect(["foo", "bar", "baz"].toLocaleString()).toBe("foo,bar,baz");
    });

    test("null and undefined result in empty strings", () => {
        expect([null].toLocaleString()).toBe("");
        expect([undefined].toLocaleString()).toBe("");
        expect([undefined, null].toLocaleString()).toBe(",");
    });

    test("empty values result in empty strings", () => {
        expect(new Array(1).toLocaleString()).toBe("");
        expect(new Array(3).toLocaleString()).toBe(",,");
        var a = new Array(5);
        a[2] = "foo";
        a[4] = "bar";
        expect(a.toLocaleString()).toBe(",,foo,,bar");
    });

    test("getter property is included in returned string", () => {
        var a = ["foo"];
        Object.defineProperty(a, 1, {
            get() {
                return "bar";
            },
        });
        expect(a.toLocaleString()).toBe("foo,bar");
    });

    test("array with elements that have a custom toString() function", () => {
        var toStringCalled = 0;
        var o = {
            toString() {
                toStringCalled++;
                return "o";
            },
        };
        expect([o, undefined, o, null, o].toLocaleString()).toBe("o,,o,,o");
        expect(toStringCalled).toBe(3);
    });

    test("array with circular references", () => {
        const a = ["foo", [], [1, 2, []], ["bar"]];
        a[1] = a;
        a[2][2] = a;
        // [ "foo", <circular>, [ 1, 2, <circular> ], [ "bar" ] ]
        expect(a.toLocaleString()).toBe("foo,,1,2,,bar");
    });

    test("with options", () => {
        expect([12, 34].toLocaleString("en")).toBe("12,34");
        expect([12, 34].toLocaleString("ar")).toBe("\u0661\u0662,\u0663\u0664");

        expect([0.234].toLocaleString("en", { style: "percent" })).toBe("23%");
        expect([0.234].toLocaleString("ar", { style: "percent" })).toBe("\u0662\u0663\u066a\u061c");
    });
});
