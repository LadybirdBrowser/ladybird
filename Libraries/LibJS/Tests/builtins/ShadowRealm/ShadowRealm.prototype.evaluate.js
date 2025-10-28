describe("normal behavior", () => {
    test("length is 1", () => {
        expect(ShadowRealm.prototype.evaluate).toHaveLength(1);
    });

    test("basic functionality", () => {
        const shadowRealm = new ShadowRealm();
        expect(shadowRealm.evaluate("globalThis.foo = 'bar';")).toBe("bar");
        expect(shadowRealm.foo).toBeUndefined();
        expect(shadowRealm.evaluate("foo;")).toBe("bar");
        expect(shadowRealm.evaluate("foo;")).toBe("bar");
    });

    test("global object initialization", () => {
        // Currently uses a plain JS::GlobalObject, i.e. no TestRunnerGlobalObject functions are available on the
        // shadow realm's global object. This may change in the future, update the test accordingly.
        const shadowRealm = new ShadowRealm();
        expect(shadowRealm.evaluate("globalThis.markAsGarbage")).toBeUndefined();
    });

    test("strict mode behavior", () => {
        const shadowRealm = new ShadowRealm();

        // sloppy mode
        expect(shadowRealm.evaluate("(function() { return !this; })()")).toBe(false);
        // strict mode
        expect(shadowRealm.evaluate("'use strict'; (function() { return !this; })()")).toBe(true);
        // Only the parsed script's strict mode changes the strictEval value used for EvalDeclarationInstantiation
        expect(
            (function () {
                "use strict";
                return shadowRealm.evaluate("(function() { return !this; })()");
            })()
        ).toBe(false);
    });

    test("wrapped function object", () => {
        const shadowRealm = new ShadowRealm();

        const string = shadowRealm.evaluate("(function () { return 'foo'; })")();
        expect(string).toBe("foo");

        const wrappedFunction = shadowRealm.evaluate("(function () { return 'foo'; })");
        expect(wrappedFunction()).toBe("foo");
        expect(typeof wrappedFunction).toBe("function");
        expect(Object.getPrototypeOf(wrappedFunction)).toBe(Function.prototype);

        expect(shadowRealm.evaluate("(function () {})").name).toBe("");
        expect(shadowRealm.evaluate("(function foo() {})").name).toBe("foo");
        expect(shadowRealm.evaluate("(function () {})")).toHaveLength(0);
        expect(shadowRealm.evaluate("(function (foo, bar) {})")).toHaveLength(2);
        expect(
            shadowRealm.evaluate(
                "Object.defineProperty(function () {}, 'length', { get() { return -Infinity } })"
            )
        ).toHaveLength(0);
        expect(
            shadowRealm.evaluate(
                "Object.defineProperty(function () {}, 'length', { get() { return Infinity } })"
            )
        ).toHaveLength(Infinity);

        for (const property of ["name", "length"]) {
            expect(() => {
                shadowRealm.evaluate(
                    `
                    function foo() {}
                    Object.defineProperty(foo, "${property}", {
                        get() { throw Error(); }
                    });
                    `
                );
            }).toThrowWithMessage(
                TypeError,
                "Trying to copy target name and length did not complete normally"
            );
        }

        expect(() => {
            shadowRealm.evaluate("(function () { throw Error(); })")();
        }).toThrowWithMessage(
            TypeError,
            "Call of wrapped target function did not complete normally"
        );
    });
});

describe("errors", () => {
    test("this value must be a ShadowRealm object", () => {
        expect(() => {
            ShadowRealm.prototype.evaluate.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type ShadowRealm");
    });

    test("throws for non-string input", () => {
        const shadowRealm = new ShadowRealm();
        const values = [
            [undefined, "undefined"],
            [42, "42"],
            [new String(), "[object StringObject]"],
        ];
        for (const [value, errorString] of values) {
            expect(() => {
                shadowRealm.evaluate(value);
            }).toThrowWithMessage(TypeError, `${errorString} is not a string`);
        }
    });

    test("throws if non-function object is returned from evaluation", () => {
        const shadowRealm = new ShadowRealm();
        const values = [
            ["[]", "[object Array]"],
            ["({})", "[object Object]"],
            ["new String()", "[object StringObject]"],
        ];
        for (const [value, errorString] of values) {
            expect(() => {
                shadowRealm.evaluate(value);
            }).toThrowWithMessage(
                TypeError,
                `Wrapped value must be primitive or a function object, got ${errorString}`
            );
        }
    });

    test("any exception is changed to a TypeError", () => {
        const shadowRealm = new ShadowRealm();
        expect(() => {
            shadowRealm.evaluate("(() => { throw 42; })()");
        }).toThrowWithMessage(TypeError, "The evaluated script did not complete normally");
    });

    test("TypeError from revoked proxy is associated to caller realm", () => {
        const shadowRealm = new ShadowRealm();
        shadowRealm.evaluate("p = Proxy.revocable(() => {}, {}); undefined");
        const proxy = shadowRealm.evaluate("p.proxy");
        const revoke = shadowRealm.evaluate("p.revoke");
        const ShadowRealmTypeError = shadowRealm.evaluate("TypeError");
        revoke();
        try {
            proxy();
            expect.fail();
        } catch (e) {
            expect(e.constructor).toBe(TypeError);
            expect(e.constructor).not.toBe(ShadowRealmTypeError);
        }
    });
});
