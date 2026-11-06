class Base {
    method() {
        return 1;
    }
}

class Derived extends Base {
    #field = 1;

    #method() {
        return this;
    }

    deleteOfSuper() {
        return delete super.method;
    }

    deleteOfSuperWithKey(key) {
        return delete super[key()];
    }

    readOptionally(object) {
        return object?.#field;
    }

    callOptionally(object) {
        return object?.#method();
    }

    callInParentheses(object) {
        return (object?.#method)();
    }

    assignToSuperInPatterns() {
        [super.a] = [1];
        ({ b: super["b"] } = { b: 2 });
        return [this.a, this.b];
    }

    assignToPrivateInPattern() {
        [this.#field] = [5];
        return this.#field;
    }

    loopOverSuperAndPrivate() {
        for (super.c of [1, 2]);
        for (super["d"] in { key: 1 });
        for (this.#field of [7]);
        return [this.c, this.d, this.#field];
    }
}

test("delete of a property of super throws after evaluating the key", () => {
    expect(() => new Derived().deleteOfSuper()).toThrowWithMessage(
        ReferenceError,
        "Can't delete a property on 'super'"
    );
    let evaluated = false;
    expect(() =>
        new Derived().deleteOfSuperWithKey(() => {
            evaluated = true;
            return "method";
        })
    ).toThrow(ReferenceError);
    expect(evaluated).toBeTrue();
});

test("private names in optional chains", () => {
    const object = new Derived();
    expect(object.readOptionally(object)).toBe(1);
    expect(object.readOptionally(null)).toBeUndefined();
    expect(object.callOptionally(object)).toBe(object);
    expect(object.callOptionally(undefined)).toBeUndefined();
    expect(object.callInParentheses(object)).toBe(object);
    expect(() => object.callInParentheses(null)).toThrow(TypeError);
});

test("super and private names as assignment targets", () => {
    expect(new Derived().assignToSuperInPatterns()).toEqual([1, 2]);
    expect(new Derived().assignToPrivateInPattern()).toBe(5);
    expect(new Derived().loopOverSuperAndPrivate()).toEqual([2, "key", 7]);
});

test("number and BigInt keys of methods and accessors", () => {
    function f() {
        return {
            1() {},
            get 2() {
                return 2;
            },
            3n() {},
            get 4n() {
                return 4;
            },
            5n: 6,
            6n: function () {},
            7n: class {
                static n = this.name;
            },
        };
    }
    const object = f();
    expect(object[1].name).toBe("1");
    expect(object[2]).toBe(2);
    expect(object[3].name).toBe("3");
    expect(Object.getOwnPropertyDescriptor(object, "4").get.name).toBe("get 4");
    expect(object[5]).toBe(6);
    expect(object[6].name).toBe("6");
    expect(object[7].n).toBe("7");
});

test("an optional chain in parentheses keeps what it gets called on", () => {
    function f() {
        const object = {
            method() {
                return this;
            },
            nested: {
                method() {
                    return this;
                },
            },
        };
        expect((object?.method)()).toBe(object);
        expect((object?.["method"])()).toBe(object);
        expect((object?.nested.method)()).toBe(object.nested);
        expect(object?.method?.()).toBe(object);
        expect((object?.method)``).toBe(object);
        const nothing = null;
        expect(() => (nothing?.method)()).toThrow(TypeError);
        expect(nothing?.method?.()).toBeUndefined();
    }
    f();
});

test("a call as a target throws after evaluating the call, and before the value", () => {
    const log = [];
    const call = () => {
        log.push("call");
        return {};
    };
    const value = () => {
        log.push("value");
    };
    const sources = [
        "call() = value();",
        "call() += value();",
        "++call();",
        "const object = {}; object.a = call()++;",
        "const object = {}; object.a = call() = value();",
        "call()-- + value();",
        "value(-(call() += value()));",
        "for (call() of [1]);",
        "for (call() in { key: 1 });",
    ];
    for (const source of sources) {
        log.length = 0;
        const f = new Function("call", "value", source);
        expect(() => f(call, value)).toThrowWithMessage(ReferenceError, "Invalid left-hand side in assignment");
        expect(log).toEqual(["call"]);
    }
    expect(new Function("call", "for (call() of []); return 1;")(call)).toBe(1);
});

test("patterns nested in a rest element", () => {
    function f() {
        let a, b;
        const object = {};
        [...[a = 1, b]] = [];
        [...[object.c = 2]] = [];
        [...[[object.d = 3] = []]] = [];
        [...{ length: object.e }] = [1, 2];
        return [a, b, object];
    }
    expect(f()).toEqual([1, undefined, { c: 2, d: 3, e: 2 }]);
});

test("a var initializer in the head of a for-in loop", () => {
    function f() {
        function read() {
            return x;
        }
        const before = read();
        for (var x = (1) in {});
        return [before, read()];
    }
    expect(f()).toEqual([undefined, 1]);

    function g() {
        const object = { p: 1 };
        with (object) {
            for (var p = (delete object.p) in {});
        }
        return [object.p, p];
    }
    expect(g()).toEqual([true, undefined]);
});

test("BigInt keys of fields and patterns", () => {
    function f() {
        class C {
            1n = 1;
            static 2n = () => {};
        }
        const { 3n: a, 0x4n: b = 5 } = { 3: 4 };
        return [new C()[1], C[2].name, a, b];
    }
    expect(f()).toEqual([1, "2", 4, 5]);
});

test("a rest element whose target is a member", () => {
    function f() {
        const object = {};
        const value = [1, 2];
        const result = ([object.first, ...object.rest] = value);
        return [result === value, object.first, object.rest];
    }
    expect(f()).toEqual([true, 1, [2]]);
});
