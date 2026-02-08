// The spec's Get(O, P) abstract operation is defined as O.[[Get]](P, O), meaning the
// receiver for property access is the object itself. When the bytecode interpreter's
// GetGlobal optimization bypasses the normal environment record lookup, it must still
// pass the global object as the receiver for accessor properties on the global object.

describe("getters on global object receive globalThis as this-value", () => {
    test("strict getter via direct reference", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(__test_g).toBe(globalThis);
        delete globalThis.__test_g;
    });

    test("sloppy getter via direct reference", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                return this;
            },
            configurable: true,
        });
        expect(__test_g).toBe(globalThis);
        delete globalThis.__test_g;
    });

    test("getter accessed multiple times exercises caching", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        // First access populates the cache, subsequent accesses use the cached path.
        expect(__test_g).toBe(globalThis);
        expect(__test_g).toBe(globalThis);
        expect(__test_g).toBe(globalThis);
        delete globalThis.__test_g;
    });

    test("typeof on global getter returns correct type", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(typeof __test_g).toBe("object");
        delete globalThis.__test_g;
    });

    test("getter accessed from eval", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(eval("__test_g")).toBe(globalThis);
        delete globalThis.__test_g;
    });

    test("getter accessed from indirect eval", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect((0, eval)("__test_g")).toBe(globalThis);
        delete globalThis.__test_g;
    });

    test("getter accessed from new Function", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(new Function("return __test_g")()).toBe(globalThis);
        delete globalThis.__test_g;
    });

    test("getter accessed from strict function scope", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        var result = (function () {
            "use strict";
            return __test_g;
        })();
        expect(result).toBe(globalThis);
        delete globalThis.__test_g;
    });

    test("getter accessed from sloppy function scope", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        var result = (function () {
            return __test_g;
        })();
        expect(result).toBe(globalThis);
        delete globalThis.__test_g;
    });

    test("getter accessed from nested function scopes", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        var result = (function () {
            return (function () {
                return __test_g;
            })();
        })();
        expect(result).toBe(globalThis);

        var arrowResult = (() => (() => __test_g)())();
        expect(arrowResult).toBe(globalThis);
        delete globalThis.__test_g;
    });

    test("getter that reads another global property via this", () => {
        globalThis.__test_helper = 42;
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                return this.__test_helper;
            },
            configurable: true,
        });
        expect(__test_g).toBe(42);
        globalThis.__test_helper = 99;
        expect(__test_g).toBe(99);
        delete globalThis.__test_g;
        delete globalThis.__test_helper;
    });

    test("getter inherited from global object's prototype", () => {
        Object.defineProperty(Object.getPrototypeOf(globalThis), "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(__test_g).toBe(globalThis);
        expect(__test_g).toBe(globalThis);
        delete Object.getPrototypeOf(globalThis).__test_g;
    });

    test("getter used in various expression positions", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });

        // Conditional expression
        expect(true ? __test_g : null).toBe(globalThis);
        expect(false ? null : __test_g).toBe(globalThis);

        // Logical operators
        expect(false || __test_g).toBe(globalThis);
        expect(__test_g || null).toBe(globalThis);
        expect(true && __test_g).toBe(globalThis);
        expect(null ?? __test_g).toBe(globalThis);

        // Comma operator
        expect((0, __test_g)).toBe(globalThis);

        // Array element
        expect([__test_g][0]).toBe(globalThis);

        // Object value
        expect({ k: __test_g }.k).toBe(globalThis);

        // Function argument
        expect(
            (function (x) {
                return x;
            })(__test_g)
        ).toBe(globalThis);

        // Arrow argument
        expect((x => x)(__test_g)).toBe(globalThis);

        // Return statement
        expect(
            (function () {
                return __test_g;
            })()
        ).toBe(globalThis);

        // Equality
        expect(__test_g == globalThis).toBeTrue();
        expect(__test_g === globalThis).toBeTrue();

        delete globalThis.__test_g;
    });

    test("getter used in control flow positions", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });

        // switch
        var switchResult = (function () {
            switch (__test_g) {
                case globalThis:
                    return true;
                default:
                    return false;
            }
        })();
        expect(switchResult).toBeTrue();

        // if
        expect(
            (function () {
                if (__test_g === globalThis) return true;
                return false;
            })()
        ).toBeTrue();

        // while
        expect(
            (function () {
                while (__test_g === globalThis) return true;
                return false;
            })()
        ).toBeTrue();

        // for
        expect(
            (function () {
                for (; __test_g === globalThis; ) return true;
                return false;
            })()
        ).toBeTrue();

        // do-while
        expect(
            (function () {
                do {
                    return __test_g === globalThis;
                } while (false);
            })()
        ).toBeTrue();

        // try/finally
        expect(
            (function () {
                try {
                    return __test_g;
                } finally {
                }
            })()
        ).toBe(globalThis);

        // throw/catch
        expect(
            (function () {
                try {
                    throw __test_g;
                } catch (e) {
                    return e;
                }
            })()
        ).toBe(globalThis);

        delete globalThis.__test_g;
    });

    test("getter used with yield", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });

        function* gen() {
            yield __test_g;
            yield __test_g;
        }
        var it = gen();
        expect(it.next().value).toBe(globalThis);
        expect(it.next().value).toBe(globalThis);

        delete globalThis.__test_g;
    });

    test("getter used as default parameter value", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });

        expect(
            (function (x = __test_g) {
                return x;
            })()
        ).toBe(globalThis);

        expect(
            (function ([x = __test_g]) {
                return x;
            })([undefined])
        ).toBe(globalThis);

        delete globalThis.__test_g;
    });

    test("getter returns value usable in property chain", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return { nested: this };
            },
            configurable: true,
        });
        expect(__test_g.nested).toBe(globalThis);
        expect(__test_g["nested"]).toBe(globalThis);
        delete globalThis.__test_g;
    });

    test("getter survives cache invalidation from data→accessor→data→accessor", () => {
        globalThis.__test_g = "value";
        expect(__test_g).toBe("value");

        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(__test_g).toBe(globalThis);

        Object.defineProperty(globalThis, "__test_g", {
            value: "back",
            configurable: true,
            writable: true,
        });
        expect(__test_g).toBe("back");

        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(__test_g).toBe(globalThis);
        delete globalThis.__test_g;
    });

    test("getter survives delete and re-add", () => {
        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(__test_g).toBe(globalThis);
        delete globalThis.__test_g;

        expect(() => {
            __test_g;
        }).toThrowWithMessage(ReferenceError, "'__test_g' is not defined");

        Object.defineProperty(globalThis, "__test_g", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(__test_g).toBe(globalThis);
        delete globalThis.__test_g;
    });

    test("multiple global getters can coexist", () => {
        Object.defineProperty(globalThis, "__test_g1", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        Object.defineProperty(globalThis, "__test_g2", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(__test_g1).toBe(globalThis);
        expect(__test_g2).toBe(globalThis);
        expect(__test_g1).toBe(__test_g2);
        delete globalThis.__test_g1;
        delete globalThis.__test_g2;
    });
});

describe("setters on global object receive globalThis as this-value", () => {
    test("strict setter via direct assignment", () => {
        var receivedThis;
        Object.defineProperty(globalThis, "__test_s", {
            set(v) {
                "use strict";
                receivedThis = this;
            },
            configurable: true,
        });
        __test_s = 1;
        expect(receivedThis).toBe(globalThis);
        delete globalThis.__test_s;
    });

    test("sloppy setter via direct assignment", () => {
        var receivedThis;
        Object.defineProperty(globalThis, "__test_s", {
            set(v) {
                receivedThis = this;
            },
            configurable: true,
        });
        __test_s = 1;
        expect(receivedThis).toBe(globalThis);
        delete globalThis.__test_s;
    });

    test("setter via eval", () => {
        var receivedThis;
        globalThis.__test_capture = function (t) {
            receivedThis = t;
        };
        Object.defineProperty(globalThis, "__test_s", {
            set(v) {
                "use strict";
                __test_capture(this);
            },
            configurable: true,
        });
        eval("__test_s = 2");
        expect(receivedThis).toBe(globalThis);
        delete globalThis.__test_s;
        delete globalThis.__test_capture;
    });

    test("setter via indirect eval", () => {
        var receivedThis;
        globalThis.__test_capture = function (t) {
            receivedThis = t;
        };
        Object.defineProperty(globalThis, "__test_s", {
            set(v) {
                "use strict";
                __test_capture(this);
            },
            configurable: true,
        });
        (0, eval)("__test_s = 3");
        expect(receivedThis).toBe(globalThis);
        delete globalThis.__test_s;
        delete globalThis.__test_capture;
    });

    test("setter that modifies another global property via this", () => {
        globalThis.__test_target = 0;
        Object.defineProperty(globalThis, "__test_s", {
            set(v) {
                this.__test_target = v;
            },
            configurable: true,
        });
        __test_s = 99;
        expect(globalThis.__test_target).toBe(99);
        delete globalThis.__test_s;
        delete globalThis.__test_target;
    });

    test("setter inherited from global object's prototype", () => {
        var receivedThis;
        Object.defineProperty(Object.getPrototypeOf(globalThis), "__test_s", {
            set(v) {
                "use strict";
                receivedThis = this;
            },
            configurable: true,
        });
        __test_s = 1;
        expect(receivedThis).toBe(globalThis);
        delete Object.getPrototypeOf(globalThis).__test_s;
    });
});

describe("compound assignment operators on global accessors", () => {
    test("arithmetic compound assignments", () => {
        var getterThis, setterThis;
        Object.defineProperty(globalThis, "__test_cs", {
            get() {
                "use strict";
                getterThis = this;
                return 10;
            },
            set(v) {
                "use strict";
                setterThis = this;
            },
            configurable: true,
        });

        var ops = [
            ["+=", () => (__test_cs += 1)],
            ["-=", () => (__test_cs -= 1)],
            ["*=", () => (__test_cs *= 2)],
            ["/=", () => (__test_cs /= 2)],
            ["%=", () => (__test_cs %= 3)],
            ["**=", () => (__test_cs **= 2)],
            ["<<=", () => (__test_cs <<= 1)],
            [">>=", () => (__test_cs >>= 1)],
            [">>>=", () => (__test_cs >>>= 1)],
            ["&=", () => (__test_cs &= 0xff)],
            ["|=", () => (__test_cs |= 0xff)],
            ["^=", () => (__test_cs ^= 0xff)],
        ];

        for (var [name, op] of ops) {
            getterThis = null;
            setterThis = null;
            op();
            expect(getterThis).toBe(globalThis);
            expect(setterThis).toBe(globalThis);
        }

        delete globalThis.__test_cs;
    });

    test("prefix and postfix increment/decrement", () => {
        var getterThis, setterThis;
        Object.defineProperty(globalThis, "__test_cs", {
            get() {
                "use strict";
                getterThis = this;
                return 10;
            },
            set(v) {
                "use strict";
                setterThis = this;
            },
            configurable: true,
        });

        getterThis = null;
        setterThis = null;
        ++__test_cs;
        expect(getterThis).toBe(globalThis);
        expect(setterThis).toBe(globalThis);

        getterThis = null;
        setterThis = null;
        __test_cs++;
        expect(getterThis).toBe(globalThis);
        expect(setterThis).toBe(globalThis);

        getterThis = null;
        setterThis = null;
        --__test_cs;
        expect(getterThis).toBe(globalThis);
        expect(setterThis).toBe(globalThis);

        getterThis = null;
        setterThis = null;
        __test_cs--;
        expect(getterThis).toBe(globalThis);
        expect(setterThis).toBe(globalThis);

        delete globalThis.__test_cs;
    });

    test("logical assignment operators", () => {
        var getterThis, setterThis;
        Object.defineProperty(globalThis, "__test_cs", {
            get() {
                "use strict";
                getterThis = this;
                return 10;
            },
            set(v) {
                "use strict";
                setterThis = this;
            },
            configurable: true,
        });

        // ||= with truthy value: getter called, setter NOT called
        getterThis = null;
        setterThis = null;
        __test_cs ||= 99;
        expect(getterThis).toBe(globalThis);
        expect(setterThis).toBeNull();

        // &&= with truthy value: getter called, setter called
        getterThis = null;
        setterThis = null;
        __test_cs &&= 99;
        expect(getterThis).toBe(globalThis);
        expect(setterThis).toBe(globalThis);

        // ??= with non-nullish value: getter called, setter NOT called
        getterThis = null;
        setterThis = null;
        __test_cs ??= 99;
        expect(getterThis).toBe(globalThis);
        expect(setterThis).toBeNull();

        delete globalThis.__test_cs;
    });
});

describe("with statement accessor this-value", () => {
    test("getter on with-object receives with-object as this", () => {
        var obj = {};
        Object.defineProperty(obj, "prop", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        var result;
        with (obj) {
            result = prop;
        }
        expect(result).toBe(obj);
    });

    test("setter on with-object receives with-object as this", () => {
        var obj = {};
        var receivedThis;
        Object.defineProperty(obj, "prop", {
            set(v) {
                "use strict";
                receivedThis = this;
            },
            configurable: true,
        });
        with (obj) {
            prop = 1;
        }
        expect(receivedThis).toBe(obj);
    });

    test("compound assignment on with-object accessor", () => {
        var obj = {};
        var getterThis, setterThis;
        Object.defineProperty(obj, "prop", {
            get() {
                "use strict";
                getterThis = this;
                return 10;
            },
            set(v) {
                "use strict";
                setterThis = this;
            },
            configurable: true,
        });
        with (obj) {
            prop += 5;
        }
        expect(getterThis).toBe(obj);
        expect(setterThis).toBe(obj);
    });

    test("increment/decrement on with-object accessor", () => {
        var obj = {};
        var getterThis, setterThis;
        Object.defineProperty(obj, "prop", {
            get() {
                "use strict";
                getterThis = this;
                return 10;
            },
            set(v) {
                "use strict";
                setterThis = this;
            },
            configurable: true,
        });

        getterThis = null;
        setterThis = null;
        with (obj) {
            ++prop;
        }
        expect(getterThis).toBe(obj);
        expect(setterThis).toBe(obj);

        getterThis = null;
        setterThis = null;
        with (obj) {
            prop++;
        }
        expect(getterThis).toBe(obj);
        expect(setterThis).toBe(obj);
    });

    test("with-statement falls back to global getter correctly", () => {
        Object.defineProperty(globalThis, "__test_gw", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        var result;
        with ({}) {
            result = __test_gw;
        }
        expect(result).toBe(globalThis);
        delete globalThis.__test_gw;
    });

    test("nested with statements resolve to correct objects", () => {
        var outer = {};
        Object.defineProperty(outer, "op", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        var inner = {};
        Object.defineProperty(inner, "ip", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        var r1, r2;
        with (outer) {
            with (inner) {
                r1 = ip;
                r2 = op;
            }
        }
        expect(r1).toBe(inner);
        expect(r2).toBe(outer);
    });

    test("with Proxy as binding object", () => {
        var target = {};
        Object.defineProperty(target, "pp", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        var proxy = new Proxy(target, {});
        var result;
        with (proxy) {
            result = pp;
        }
        expect(result).toBe(proxy);
    });

    test("with Proxy get handler receives proxy as receiver", () => {
        var receivedReceiver;
        var proxy = new Proxy(
            {},
            {
                has(t, p) {
                    return p === "pp";
                },
                get(t, p, receiver) {
                    receivedReceiver = receiver;
                    return 99;
                },
            }
        );
        with (proxy) {
            pp;
        }
        expect(receivedReceiver).toBe(proxy);
    });

    test("with + eval resolves getter correctly", () => {
        var obj = {};
        Object.defineProperty(obj, "ep", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        var result;
        with (obj) {
            result = eval("ep");
        }
        expect(result).toBe(obj);
    });
});

describe("for-in/for-of with global setter targets", () => {
    test("for-of assigns to global setter with correct this", () => {
        var receivedThis;
        Object.defineProperty(globalThis, "__test_fs", {
            set(v) {
                "use strict";
                receivedThis = this;
            },
            configurable: true,
        });
        for (__test_fs of [1]) {
        }
        expect(receivedThis).toBe(globalThis);
        delete globalThis.__test_fs;
    });

    test("for-in assigns to global setter with correct this", () => {
        var receivedThis;
        Object.defineProperty(globalThis, "__test_fs", {
            set(v) {
                "use strict";
                receivedThis = this;
            },
            configurable: true,
        });
        for (__test_fs in { a: 1 }) {
        }
        expect(receivedThis).toBe(globalThis);
        delete globalThis.__test_fs;
    });
});

describe("destructuring assignment to global setters", () => {
    test("array destructuring", () => {
        var receivedThisA, receivedThisB;
        Object.defineProperty(globalThis, "__test_da", {
            set(v) {
                "use strict";
                receivedThisA = this;
            },
            configurable: true,
        });
        Object.defineProperty(globalThis, "__test_db", {
            set(v) {
                "use strict";
                receivedThisB = this;
            },
            configurable: true,
        });
        [__test_da, __test_db] = [1, 2];
        expect(receivedThisA).toBe(globalThis);
        expect(receivedThisB).toBe(globalThis);
        delete globalThis.__test_da;
        delete globalThis.__test_db;
    });

    test("object destructuring", () => {
        var receivedThisA, receivedThisB;
        Object.defineProperty(globalThis, "__test_da", {
            set(v) {
                "use strict";
                receivedThisA = this;
            },
            configurable: true,
        });
        Object.defineProperty(globalThis, "__test_db", {
            set(v) {
                "use strict";
                receivedThisB = this;
            },
            configurable: true,
        });
        ({ x: __test_da, y: __test_db } = { x: 1, y: 2 });
        expect(receivedThisA).toBe(globalThis);
        expect(receivedThisB).toBe(globalThis);
        delete globalThis.__test_da;
        delete globalThis.__test_db;
    });

    test("array rest destructuring", () => {
        var receivedThisA, receivedThisB;
        Object.defineProperty(globalThis, "__test_da", {
            set(v) {
                "use strict";
                receivedThisA = this;
            },
            configurable: true,
        });
        Object.defineProperty(globalThis, "__test_db", {
            set(v) {
                "use strict";
                receivedThisB = this;
            },
            configurable: true,
        });
        [__test_da, ...__test_db] = [1, 2, 3];
        expect(receivedThisA).toBe(globalThis);
        expect(receivedThisB).toBe(globalThis);
        delete globalThis.__test_da;
        delete globalThis.__test_db;
    });
});

describe("primitive base value getters receive primitive as this in strict mode", () => {
    test("string primitive", () => {
        Object.defineProperty(String.prototype, "__test_pt", {
            get() {
                "use strict";
                return typeof this;
            },
            configurable: true,
        });
        expect("hello".__test_pt).toBe("string");
        delete String.prototype.__test_pt;
    });

    test("number primitive", () => {
        Object.defineProperty(Number.prototype, "__test_pt", {
            get() {
                "use strict";
                return typeof this;
            },
            configurable: true,
        });
        expect((42).__test_pt).toBe("number");
        delete Number.prototype.__test_pt;
    });

    test("boolean primitive", () => {
        Object.defineProperty(Boolean.prototype, "__test_pt", {
            get() {
                "use strict";
                return typeof this;
            },
            configurable: true,
        });
        expect(true.__test_pt).toBe("boolean");
        delete Boolean.prototype.__test_pt;
    });

    test("symbol primitive", () => {
        Object.defineProperty(Symbol.prototype, "__test_pt", {
            get() {
                "use strict";
                return typeof this;
            },
            configurable: true,
        });
        expect(Symbol("test").__test_pt).toBe("symbol");
        delete Symbol.prototype.__test_pt;
    });

    test("bigint primitive", () => {
        Object.defineProperty(BigInt.prototype, "__test_pt", {
            get() {
                "use strict";
                return typeof this;
            },
            configurable: true,
        });
        expect(42n.__test_pt).toBe("bigint");
        delete BigInt.prototype.__test_pt;
    });

    test("string primitive getter boxes this in sloppy mode", () => {
        Object.defineProperty(String.prototype, "__test_pt", {
            get() {
                return typeof this;
            },
            configurable: true,
        });
        expect("hello".__test_pt).toBe("object");
        delete String.prototype.__test_pt;
    });
});

describe("super property access receiver", () => {
    test("super getter receives the actual object as this", () => {
        var receivedThis;
        var base = {
            get prop() {
                "use strict";
                receivedThis = this;
                return 42;
            },
        };
        var derived = {
            __proto__: base,
            method() {
                return super.prop;
            },
        };
        receivedThis = null;
        derived.method();
        expect(receivedThis).toBe(derived);
    });

    test("super getter on grandchild receives grandchild as this", () => {
        var receivedThis;
        var base = {
            get prop() {
                "use strict";
                receivedThis = this;
                return 42;
            },
        };
        var derived = {
            __proto__: base,
            method() {
                return super.prop;
            },
        };
        var grandchild = Object.create(derived);
        receivedThis = null;
        grandchild.method();
        expect(receivedThis).toBe(grandchild);
    });

    test("super setter receives the actual object as this", () => {
        var receivedThis;
        var base = {};
        Object.defineProperty(base, "prop", {
            set(v) {
                "use strict";
                receivedThis = this;
            },
            configurable: true,
        });
        var derived = {
            __proto__: base,
            method() {
                super.prop = 1;
            },
        };
        receivedThis = null;
        derived.method();
        expect(receivedThis).toBe(derived);
    });

    test("class super getter receives instance as this", () => {
        class Base {
            get prop() {
                "use strict";
                return this;
            }
        }
        class Derived extends Base {
            getSuperProp() {
                return super.prop;
            }
        }
        var inst = new Derived();
        expect(inst.getSuperProp()).toBe(inst);
    });
});

describe("Proxy get trap receiver", () => {
    test("proxy get trap receives proxy as receiver", () => {
        var receivedReceiver;
        var proxy = new Proxy(
            { x: 1 },
            {
                get(t, p, receiver) {
                    receivedReceiver = receiver;
                    return Reflect.get(t, p, receiver);
                },
            }
        );
        proxy.x;
        expect(receivedReceiver).toBe(proxy);
    });

    test("proxy target getter receives proxy as this when forwarded", () => {
        var target = {};
        Object.defineProperty(target, "prop", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        var proxy = new Proxy(target, {});
        expect(proxy.prop).toBe(proxy);
    });

    test("proxy as prototype: get trap receives child as receiver", () => {
        var receivedReceiver;
        var proxy = new Proxy(
            { x: 1 },
            {
                get(t, p, receiver) {
                    receivedReceiver = receiver;
                    return Reflect.get(t, p, receiver);
                },
            }
        );
        var child = Object.create(proxy);
        child.x;
        expect(receivedReceiver).toBe(child);
    });
});

describe("Reflect.get/set receiver", () => {
    test("Reflect.get with explicit receiver", () => {
        var obj = {};
        Object.defineProperty(obj, "prop", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        var receiver = {};
        expect(Reflect.get(obj, "prop", receiver)).toBe(receiver);
    });

    test("Reflect.get with default receiver", () => {
        var obj = {};
        Object.defineProperty(obj, "prop", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(Reflect.get(obj, "prop")).toBe(obj);
    });

    test("Reflect.set with explicit receiver", () => {
        var receivedThis;
        var obj = {};
        Object.defineProperty(obj, "prop", {
            set(v) {
                "use strict";
                receivedThis = this;
            },
            configurable: true,
        });
        var receiver = {};
        Reflect.set(obj, "prop", 1, receiver);
        expect(receivedThis).toBe(receiver);
    });
});

describe("class static and instance accessors", () => {
    test("static getter receives the class as this", () => {
        class C {
            static get prop() {
                "use strict";
                return this;
            }
        }
        expect(C.prop).toBe(C);
    });

    test("instance getter receives the instance as this", () => {
        class C {
            get prop() {
                "use strict";
                return this;
            }
        }
        expect(new C().prop instanceof C).toBeTrue();
    });
});

describe("__defineGetter__/__defineSetter__", () => {
    test("__defineGetter__ on regular object", () => {
        var obj = {};
        obj.__defineGetter__("prop", function () {
            "use strict";
            return this;
        });
        expect(obj.prop).toBe(obj);
    });

    test("__defineGetter__ on global object", () => {
        globalThis.__defineGetter__("__test_dgg", function () {
            "use strict";
            return this;
        });
        expect(__test_dgg).toBe(globalThis);
        delete globalThis.__test_dgg;
    });

    test("__defineSetter__ on regular object", () => {
        var obj = {};
        var receivedThis;
        obj.__defineSetter__("prop", function (v) {
            "use strict";
            receivedThis = this;
        });
        obj.prop = 1;
        expect(receivedThis).toBe(obj);
    });
});

describe("accessor on special objects", () => {
    test("getter on arguments object", () => {
        function f(a, b) {
            Object.defineProperty(arguments, "2", {
                get() {
                    "use strict";
                    return this;
                },
                configurable: true,
            });
            return arguments[2] === arguments;
        }
        expect(f(1, 2)).toBeTrue();
    });

    test("getter on frozen object", () => {
        var obj = {};
        Object.defineProperty(obj, "prop", {
            get() {
                "use strict";
                return this;
            },
            configurable: false,
        });
        Object.freeze(obj);
        expect(obj.prop).toBe(obj);
    });

    test("getter on array indexed property", () => {
        var arr = [];
        Object.defineProperty(arr, "0", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(arr[0]).toBe(arr);
    });

    test("getter on Map instance", () => {
        var map = new Map();
        Object.defineProperty(Map.prototype, "__test_mp", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(map.__test_mp).toBe(map);
        delete Map.prototype.__test_mp;
    });

    test("getter on String wrapper object", () => {
        var sw = new String("hello");
        Object.defineProperty(sw, "custom", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(sw.custom).toBe(sw);
    });

    test("getter on Number wrapper object", () => {
        var nw = new Number(42);
        Object.defineProperty(nw, "custom", {
            get() {
                "use strict";
                return this;
            },
            configurable: true,
        });
        expect(nw.custom).toBe(nw);
    });
});
