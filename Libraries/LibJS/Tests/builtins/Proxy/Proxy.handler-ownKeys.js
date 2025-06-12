// TODO: Add "[[OwnPropertyKeys]] trap normal behavior" tests

describe("[[OwnPropertyKeys]] invariants", () => {
    // TODO: Add tests for other [[OwnPropertyKeys]] trap invariants

    test("cannot report new property of non-extensible object", () => {
        const target = Object.preventExtensions({});
        const handler = {
            ownKeys() {
                return ["foo", "bar", "baz"];
            },
        };
        const proxy = new Proxy(target, handler);

        expect(() => {
            Reflect.ownKeys(proxy);
        }).toThrowWithMessage(
            TypeError,
            "Proxy handler's ownKeys trap violates invariant: cannot report new property 'foo' of non-extensible object"
        );
    });

    test("cannot skip property of non-extensible object", () => {
        const target = Object.preventExtensions({ foo: null });
        const handler = {
            ownKeys() {
                return ["bar", "baz"];
            },
        };
        const proxy = new Proxy(target, handler);

        expect(() => {
            Reflect.ownKeys(proxy);
        }).toThrowWithMessage(
            TypeError,
            "Proxy handler's ownKeys trap violates invariant: cannot skip property 'foo' of non-extensible object"
        );
    });

    test("cannot skip non-configurable property", () => {
        const target = Object.defineProperty({}, "foo", { configurable: false });
        const handler = {
            ownKeys() {
                return ["bar", "baz"];
            },
        };
        const proxy = new Proxy(target, handler);

        expect(() => {
            Reflect.ownKeys(proxy);
        }).toThrowWithMessage(
            TypeError,
            "Proxy handler's ownKeys trap violates invariant: cannot skip non-configurable property 'foo'"
        );
    });
});
