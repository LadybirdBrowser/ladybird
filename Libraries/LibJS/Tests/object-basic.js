describe("correct behavior", () => {
    test("numeric indexing", () => {
        const o = { 1: 23 };

        expect(o[1]).toBe(23);
        expect(o[1n]).toBe(23);
        expect(o["1"]).toBe(23);

        o[10] = "123";
        expect(o[10]).toBe("123");
        expect(o["10"]).toBe("123");

        o[10n] = "1234";
        expect(o[10]).toBe("1234");
        expect(o["10"]).toBe("1234");
    });

    test("string indexing", () => {
        let foo = "bar";

        const o = {
            foo,
            bar: "baz",
            qux: true ? 10 : 20,
            hello: "friends",
        };

        expect(o.foo).toBe("bar");
        expect(o["foo"]).toBe("bar");
        expect(o.qux).toBe(10), expect(o.hello).toBe("friends");
        expect(o["hello"]).toBe("friends");
    });

    test("symbol keys", () => {
        let object = {};
        let symbol = Symbol("foo");

        object[symbol] = 2;
        expect(object[symbol]).toBe(2);
    });

    test("numeric keys", () => {
        const hex = { 0x10: "16" };
        const oct = { 0o10: "8" };
        const bin = { 0b10: "2" };
        const float = { 0.5: "0.5" };

        expect(hex["16"]).toBe("16");
        expect(oct["8"]).toBe("8");
        expect(bin["2"]).toBe("2");
        expect(float["0.5"]).toBe("0.5");
    });

    test("computed properties", () => {
        const foo = "bar";
        const computed = "computed";
        const o = {
            [1 + 2]: 42,
            [`I am a ${computed} key`]: foo,
        };

        expect(o[3]).toBe(42);
        expect(o["I am a computed key"]).toBe("bar");
    });

    test("duplicate keys", () => {
        const o = {
            duplicate: "hello",
            duplicate: "world",
        };
        expect(o.duplicate).toBe("world");
    });

    test("assigning after creation", () => {
        const o = {};
        o.baz = "test";

        expect(o.baz).toBe("test");
        expect(o["baz"]).toBe("test");

        expect(o[-1]).toBeUndefined();
        o[-1] = "hello friends";
        expect(o[-1]).toBe("hello friends");
        expect(o["-1"]).toBe("hello friends");
    });

    test("floating point keys", () => {
        const math = { 3.14: "pi" };
        expect(math["3.14"]).toBe("pi");
        expect(math[3.14]).toBe("pi");
    });

    test("keywords as property keys", () => {
        const o2 = {
            return: 1,
            yield: 1,
            for: 1,
            catch: 1,
            break: 1,
        };

        expect(o2.return).toBe(1);
        expect(o2.yield).toBe(1);
        expect(o2.for).toBe(1);
        expect(o2.catch).toBe(1);
        expect(o2.break).toBe(1);
    });

    test("prototypical inheritance", () => {
        var base = {
            getNumber() {
                return 10;
            },
        };

        var derived = {
            getNumber() {
                return 20 + super.getNumber();
            },
        };

        Object.setPrototypeOf(derived, base);
        expect(derived.getNumber()).toBe(30);
    });

    test("assigning object expression with destination referenced in object expression", () => {
        function go(i) {
            var i = { f: i };
            return i;
        }
        expect(go("foo")).toEqual({ f: "foo" });
    });
});

describe("side effects", () => {
    let a;
    const append = x => {
        a.push(x);
    };

    test("computed key side effects", () => {
        a = [];
        const o3 = { [append(1)]: 1, [append(2)]: 2, [append(3)]: 3 };
        expect(a).toHaveLength(3);
        expect(a[0]).toBe(1);
        expect(a[1]).toBe(2);
        expect(a[2]).toBe(3);
        expect(o3.undefined).toBe(3);
    });

    test("value side effects", () => {
        a = [];
        const o4 = { test: append(1), test: append(2), test: append(3) };
        expect(a).toHaveLength(3);
        expect(a[0]).toBe(1);
        expect(a[1]).toBe(2);
        expect(a[2]).toBe(3);
        expect(o4.test).toBeUndefined();
    });
});

describe("shorthanded properties with special names", () => {
    test("keywords cannot be used", () => {
        expect("({ function, })").not.toEval();
        expect("({ var, })").not.toEval();
        expect("({ const, })").not.toEval();
        expect("({ class, })").not.toEval();
    });

    test("reserved words are allowed in non-strict mode", () => {
        {
            var implements = 3;
            expect({ implements }).toEqual({ implements: 3 });
        }
        {
            var public = "a";
            expect({ public }).toEqual({ public: "a" });
        }
        {
            var let = 9;
            expect({ let }).toEqual({ let: 9 });
        }
        {
            var await = 8;
            expect({ await }).toEqual({ await: 8 });
        }
        {
            var async = 7;
            expect({ async }).toEqual({ async: 7 });
        }
        {
            var yield = 6;
            expect({ yield }).toEqual({ yield: 6 });
        }
    });

    test("reserved words are not allowed in strict mode", () => {
        expect('"use strict"; var implements = 3; ({ implements })').not.toEval();
        expect("\"use strict\"; var public = 'a'; ({ public })").not.toEval();
        expect('"use strict"; var let = 9; ({ let, })').not.toEval();
        expect('"use strict"; var yield = 6; ({ yield, })').not.toEval();
    });

    test("special non reserved words are allowed even in strict mode", () => {
        expect('"use strict"; var await = 8; ({ await, })').toEval();
        expect('"use strict"; var async = 7; ({ async, })').toEval();
    });
});

describe("errors", () => {
    test("syntax errors", () => {
        expect("({ foo: function() { super.bar; } })").not.toEval();
        expect("({ get ...foo })").not.toEval();
        expect("({ get... foo })").not.toEval();
        expect("({ get foo })").not.toEval();
        expect("({ get foo: bar })").not.toEval();
        expect("({ get [foo]: bar })").not.toEval();
        expect("({ get ...[foo] })").not.toEval();
        expect("({ get foo(bar) {} })").not.toEval();
        expect("({ set foo() {} })").not.toEval();
        expect("({ set foo(...bar) {} })").not.toEval();
        expect("({ set foo(bar, baz) {} })").not.toEval();
        expect("({ ...foo: bar })").not.toEval();
    });
});

describe("naming of anon functions", () => {
    test("method has name", () => {
        expect({ func() {} }.func.name).toBe("func");
    });

    test("getter has name", () => {
        expect(Object.getOwnPropertyDescriptor({ get func() {} }, "func").get.name).toBe(
            "get func"
        );
    });

    test("setter has name", () => {
        expect(Object.getOwnPropertyDescriptor({ set func(v) {} }, "func").set.name).toBe(
            "set func"
        );
    });

    test("anon function property", () => {
        expect({ func: function () {} }.func.name).toBe("func");
    });

    test("anon function from within parenthesis", () => {
        expect({ func: function () {} }.func.name).toBe("func");
    });

    test("anon function from indirect expression", () => {
        expect({ func: (0, function () {}) }.func.name).toBe("");
    });

    test("function from function call does not get named", () => {
        function f() {
            return function () {};
        }

        expect(f().name).toBe("");
        expect({ func: f() }.func.name).toBe("");
    });
});
