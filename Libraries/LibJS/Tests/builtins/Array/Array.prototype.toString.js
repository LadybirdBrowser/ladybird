test("length is 0", () => {
    expect(Array.prototype.toString).toHaveLength(0);
});

describe("normal behavior", () => {
    test("array with no elements", () => {
        expect([].toString()).toBe("");
    });

    test("array with one element", () => {
        expect([1].toString()).toBe("1");
    });

    test("array with multiple elements", () => {
        expect([1, 2, 3].toString()).toBe("1,2,3");
    });

    test("string and array concatenation", () => {
        expect("rgb(" + [10, 11, 12] + ")").toBe("rgb(10,11,12)");
    });

    test("null and undefined result in empty strings", () => {
        expect([null].toString()).toBe("");
        expect([undefined].toString()).toBe("");
        expect([undefined, null].toString()).toBe(",");
    });

    test("empty values result in empty strings", () => {
        expect(new Array(1).toString()).toBe("");
        expect(new Array(3).toString()).toBe(",,");
        var a = new Array(5);
        a[2] = "foo";
        a[4] = "bar";
        expect(a.toString()).toBe(",,foo,,bar");
    });

    test("getter property is included in returned string", () => {
        var a = [1, 2, 3];
        Object.defineProperty(a, 3, {
            get() {
                return 10;
            },
        });
        expect(a.toString()).toBe("1,2,3,10");
    });

    test("array with elements that have a custom toString() function", () => {
        var toStringCalled = 0;
        var o = {
            toString() {
                toStringCalled++;
                return "o";
            },
        };
        expect([o, undefined, o, null, o].toString()).toBe("o,,o,,o");
        expect(toStringCalled).toBe(3);
    });

    test("array with circular references", () => {
        const a = ["foo", [], [1, 2, []], ["bar"]];
        a[1] = a;
        a[2][2] = a;
        // [ "foo", <circular>, [ 1, 2, <circular> ], [ "bar" ] ]
        expect(a.toString()).toBe("foo,,1,2,,bar");
    });

    test("this value object remains the same in the %Object.prototype.toString% fallback", () => {
        let arrayPrototypeToStringThis;
        let objectPrototypeToStringThis;

        // Inject a Proxy into the Number prototype chain, so we can
        // observe Get() operations on the different object created
        // from the primitive number value.
        Number.prototype.__proto__ = new Proxy(
            {},
            {
                get(target, prop, receiver) {
                    // In Array.prototype.toString():
                    // 2. Let func be ? Get(array, "join").
                    if (prop === "join") {
                        arrayPrototypeToStringThis = receiver;
                    }

                    // In Object.prototype.toString():
                    // 15. Let tag be ? Get(O, @@toStringTag).
                    if (prop === Symbol.toStringTag) {
                        objectPrototypeToStringThis = receiver;
                    }
                },
            }
        );

        Array.prototype.toString.call(123);
        expect(typeof arrayPrototypeToStringThis).toBe("object");
        expect(typeof objectPrototypeToStringThis).toBe("object");
        expect(arrayPrototypeToStringThis).toBe(objectPrototypeToStringThis);
    });
});
