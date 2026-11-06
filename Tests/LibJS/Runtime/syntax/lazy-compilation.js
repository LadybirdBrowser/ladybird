// NB: Functions get parsed again on their first call, on their own, from what the first parse of the source recorded.

test("early errors in functions that never run are reported right away", () => {
    expect(() => eval("function outer() { function inner() { let a; let a; } }")).toThrow(SyntaxError);
    expect(() => eval("function outer() { return () => { break; }; }")).toThrow(SyntaxError);
    expect(() => eval("class C { method() { this.#missing; } }")).toThrow(SyntaxError);
    expect(() => new Function("function inner(a = 1) { 'use strict'; }")).toThrow(SyntaxError);
    expect(() => eval("function outer() { function inner() { return 1 +; } }")).toThrow(SyntaxError);
});

test("source text of functions that never ran", () => {
    function outer(a, b = () => a) {
        return function inner() {
            return a;
        };
    }
    class C {
        field = () => 1;
        method(a) {
            return a;
        }
        static block() {}
    }
    expect(outer.toString().includes("function outer(a, b = () => a)")).toBeTrue();
    expect(outer(1).toString().includes("function inner()")).toBeTrue();
    expect(new C().field.toString()).toBe("() => 1");
    expect(C.prototype.method.toString().includes("method(a)")).toBeTrue();
    expect(C.block.toString()).toBe("block() {}");
    expect(C.toString().includes("class C")).toBeTrue();
});

test("functions in the parameters of arrow functions", () => {
    function outer() {
        const first = (
            a = function () {
                return 1;
            },
            b = () => 2
        ) => a() + b();
        const second = ({ c = () => 3 }, d = async () => 4) => c();
        return first() + second({});
    }
    expect(outer()).toBe(6);
});

test("functions at the very end of a source", () => {
    expect(eval("(function () { return 1; })")()).toBe(1);
    expect(eval("x => x + 1")(1)).toBe(2);
    expect(eval("() => 1")()).toBe(1);
    expect(eval("async () => 1")()).toBeInstanceOf(Promise);
    expect(eval("(class { value = 2 })").name).toBe("");
    expect(new (eval("(class { value = 2 })"))().value).toBe(2);
    expect(new (eval("(class extends Array {})"))()).toBeInstanceOf(Array);
    expect(eval("(class { static value = 3; static { this.other = 4; } })").other).toBe(4);
    expect(eval("/* a long way to go */ (a, b) => a + b // and some more\n  ")(1, 2)).toBe(3);
});

test("names of the code around a function", () => {
    var captured = 1;
    let outerLet = 2;
    const outerConst = 3;
    function first() {
        function second() {
            function third() {
                captured++;
                return captured + outerLet + outerConst;
            }
            return third();
        }
        return second();
    }
    expect(first()).toBe(7);
    expect(captured).toBe(2);

    function constAssignment() {
        return () => {
            outerConst = 1;
        };
    }
    expect(constAssignment()).toThrow(TypeError);
});

test("functions in default parameters", () => {
    function outer(
        a = () => b,
        b = 2,
        c = function () {
            return () => a() + b;
        }
    ) {
        var b = 10;
        return [a(), c()(), b];
    }
    expect(outer()).toEqual([2, 4, 10]);

    let nested = (a = (b = (c = () => 3) => c()) => b()) => a();
    expect(nested()).toBe(3);
});

test("arrow functions take what they use from the function around them", () => {
    function outer() {
        const read = () => [this.value, arguments[0], new.target];
        return () => read();
    }
    expect(outer.call({ value: 1 }, 2)()).toEqual([1, 2, undefined]);

    class Base {
        value() {
            return 1;
        }
    }
    class Derived extends Base {
        constructor() {
            const init = () => super();
            init();
        }
        value() {
            return (() => super.value() + 1)();
        }
    }
    expect(new Derived().value()).toBe(2);
});

test("eval and with around a function keep names from being what they look like", () => {
    function sloppy() {
        eval("var injected = 1");
        function inner() {
            return injected;
        }
        return inner();
    }
    expect(sloppy()).toBe(1);

    const object = { value: 1 };
    const code = "with (object) { var read = function () { return value; }; } read";
    expect(eval(code)()).toBe(1);

    function parameterEval(before = () => injectedInParameters, after = (eval("var injectedInParameters = 2"), 0)) {
        return before();
    }
    expect(parameterEval()).toBe(2);
});

test("the code of a class", () => {
    let counter = 0;
    const key = "computed";
    class C {
        #secret = 1;
        field = ++counter;
        [key] = this.#secret + 1;
        anonymous = function () {};
        arrow = () => this;
        static staticField = "static";
        static {
            this.fromBlock = this.staticField + counter;
            let local = 1;
            this.closure = () => local++;
        }
        reveal() {
            return this.#secret;
        }
    }
    const instance = new C();
    expect(instance.field).toBe(1);
    expect(instance.computed).toBe(2);
    expect(instance.anonymous.name).toBe("anonymous");
    expect(instance.arrow()).toBe(instance);
    expect(instance.reveal()).toBe(1);
    expect(C.fromBlock).toBe("static0");
    expect(C.closure()).toBe(1);
    expect(C.closure()).toBe(2);
    expect(new C().field).toBe(2);
});

test("named function expressions and block functions", () => {
    const factorial = function self(n) {
        return n <= 1 ? 1 : n * self(n - 1);
    };
    expect(factorial(4)).toBe(24);

    const shadowed = function self() {
        var self = 1;
        return self;
    };
    expect(shadowed()).toBe(1);

    function annexB() {
        {
            function hoisted() {
                return 1;
            }
        }
        return () => hoisted();
    }
    expect(annexB()()).toBe(1);
});

test("generators and async functions", () => {
    function* generator(a) {
        const inner = () => a;
        yield inner();
        yield* [2];
    }
    expect([...generator(1)]).toEqual([1, 2]);

    async function asyncFunction() {
        return await (async () => 1)();
    }
    expect(asyncFunction()).toBeInstanceOf(Promise);
});

test("a long chain of operators takes no stack to generate", () => {
    const long = new Function("return " + "1+".repeat(500000) + "1");
    expect(long()).toBe(500001);
});

test("code too deep for the stack that is left compiles once there is enough of it", () => {
    const deep = new Function("return " + "(".repeat(200) + "1" + ")".repeat(200));
    function atTheEndOfTheStack() {
        try {
            return atTheEndOfTheStack();
        } catch (error) {
            return deep();
        }
    }
    expect(atTheEndOfTheStack()).toBe(1);
});
