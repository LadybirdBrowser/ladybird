test("length is 2", () => {
    expect(Proxy.revocable).toHaveLength(2);
});

describe("errors", () => {
    test("constructor argument count", () => {
        expect(() => {
            Proxy.revocable();
        }).toThrowWithMessage(
            TypeError,
            "Expected target argument of Proxy constructor to be object, got undefined"
        );

        expect(() => {
            Proxy.revocable({});
        }).toThrowWithMessage(
            TypeError,
            "Expected handler argument of Proxy constructor to be object, got undefined"
        );
    });

    test("constructor requires objects", () => {
        expect(() => {
            Proxy.revocable(1, {});
        }).toThrowWithMessage(
            TypeError,
            "Expected target argument of Proxy constructor to be object, got 1"
        );

        expect(() => {
            Proxy.revocable({}, 1);
        }).toThrowWithMessage(
            TypeError,
            "Expected handler argument of Proxy constructor to be object, got 1"
        );
    });
});

describe("normal behavior", () => {
    test("returns object with 'proxy' and 'revoke' properties", () => {
        const revocable = Proxy.revocable(
            {},
            {
                get() {
                    return 42;
                },
            }
        );
        expect(typeof revocable).toBe("object");
        expect(Object.getPrototypeOf(revocable)).toBe(Object.prototype);
        expect(revocable.hasOwnProperty("proxy")).toBeTrue();
        expect(revocable.hasOwnProperty("revoke")).toBeTrue();
        expect(typeof revocable.revoke).toBe("function");
        // Can't `instanceof Proxy`, but this should do the trick :^)
        expect(revocable.proxy.foo).toBe(42);
    });

    test("'revoke' function revokes Proxy", () => {
        const revocable = Proxy.revocable({}, {});
        expect(revocable.proxy.foo).toBeUndefined();
        expect(revocable.revoke()).toBeUndefined();
        expect(() => {
            revocable.proxy.foo;
        }).toThrowWithMessage(TypeError, "An operation was performed on a revoked Proxy object");
    });

    test("'revoke' called multiple times is a noop", () => {
        const revocable = Proxy.revocable({}, {});
        expect(revocable.revoke()).toBeUndefined();
        expect(revocable.revoke()).toBeUndefined();
    });
});
