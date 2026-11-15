test("class expression as default value in array destructuring", () => {
    var x;
    [x = class {}] = [undefined];
    expect(typeof x).toBe("function");
});

test("class expression as default value in object destructuring", () => {
    var x;
    ({ x = class {} } = {});
    expect(typeof x).toBe("function");
});

test("class expression as default value in for-of array destructuring", () => {
    for (var [x = class C {}] of [[undefined]]) {
        expect(typeof x).toBe("function");
    }
});

test("function expression as default value in array destructuring", () => {
    var x;
    [x = function () {}] = [undefined];
    expect(typeof x).toBe("function");
});

test("nested function as default value in array destructuring", () => {
    var x;
    [
        x = function () {
            return function inner() {};
        },
    ] = [undefined];
    expect(typeof x).toBe("function");
    expect(typeof x()).toBe("function");
});

test("MemberExpression target in array destructuring", () => {
    var obj = {};
    [obj.x] = [42];
    expect(obj.x).toBe(42);
});

test("MemberExpression target with object literal in array destructuring", () => {
    var setValue;
    [
        {
            set y(val) {
                setValue = val;
            },
        }.y,
    ] = [23];
    expect(setValue).toBe(23);
});

test("MemberExpression target with object literal in object destructuring", () => {
    var setValue;
    ({
        x: {
            set y(val) {
                setValue = val;
            },
        }.y,
    } = { x: 42 });
    expect(setValue).toBe(42);
});

test("object destructuring evaluates MemberExpression target before source property", () => {
    var log = [];
    var source = {
        get p() {
            log.push("get p");
            return 42;
        },
    };
    var target = {
        set q(value) {
            log.push(`set q ${value}`);
        },
    };

    ({ p: (log.push("target base"), target).q } = source);
    expect(log).toEqual(["target base", "get p", "set q 42"]);
});

test("object destructuring evaluates computed MemberExpression target before source property", () => {
    var log = [];
    var source = {
        get p() {
            log.push("get p");
            return undefined;
        },
    };
    var target = {
        set q(value) {
            log.push(`set q ${value}`);
        },
    };
    function sourceKey() {
        log.push("source key");
        return {
            toString() {
                log.push("source key toString");
                return "p";
            },
        };
    }
    function targetKey() {
        log.push("target key");
        return "q";
    }

    ({ [sourceKey()]: target[targetKey()] = (log.push("default"), 7) } = source);
    expect(log).toEqual(["source key", "source key toString", "target key", "get p", "default", "set q 7"]);
});

test("object rest destructuring evaluates MemberExpression target before copying properties", () => {
    var log = [];
    var source = {
        a: 1,
        get b() {
            log.push("get b");
            return 2;
        },
    };
    var target = {};

    ({ a, ...(log.push("target base"), target).rest } = source);
    expect(log).toEqual(["target base", "get b"]);
    expect(target.rest).toEqual({ b: 2 });
});

test("named class expression as default value in array destructuring", () => {
    var x;
    [x = class C {}] = [undefined];
    expect(typeof x).toBe("function");
    expect(x.name).toBe("C");
});

test("class with method referencing class name in destructuring default", () => {
    var x;
    [
        x = class C {
            static getName() {
                return C.name;
            }
        },
    ] = [undefined];
    expect(x.getName()).toBe("C");
});

test("arrow function as default value in array destructuring", () => {
    var x;
    [x = () => 42] = [undefined];
    expect(x()).toBe(42);
});

test("arrow function capturing outer variable in destructuring default", () => {
    var outer = "hello";
    var x;
    [x = () => outer] = [undefined];
    expect(x()).toBe("hello");
});

test("nested destructuring with class expression defaults", () => {
    var a, b;
    [a = class {}, [b = class {}]] = [undefined, [undefined]];
    expect(typeof a).toBe("function");
    expect(typeof b).toBe("function");
});

test("object destructuring with function expression accessing parameter", () => {
    var x;
    ({
        x = function f(n) {
            return n > 0 ? n * f(n - 1) : 1;
        },
    } = {});
    expect(x(5)).toBe(120);
});

test("default value with eval in array destructuring", () => {
    var x;
    [x = eval("42")] = [undefined];
    expect(x).toBe(42);
});

test("array destructuring default value does not leak class name", () => {
    var x;
    [x = class C {}] = [undefined];
    expect(() => C).toThrowWithMessage(ReferenceError, "'C' is not defined");
});

test("parenthesized targets with defaults", () => {
    expect(new Function("var target; [(target) = 5] = [1]; return target;")()).toBe(1);
    expect(new Function("var target; [(target) = 5] = []; return target;")()).toBe(5);
});

test("default that overwrites the base of its MemberExpression target", () => {
    function objectPattern(source) {
        let target = {};
        const original = target;
        ({ x: target.foo = ((target = {}), 7) } = source);
        return [original, target];
    }
    function arrayPattern(source) {
        let target = {};
        const original = target;
        [target.foo = ((target = {}), 7)] = source;
        return [original, target];
    }

    for (const [original, target] of [objectPattern({ x: 3 }), arrayPattern([3])]) {
        expect(target).toBe(original);
        expect(original.foo).toBe(3);
    }
    for (const [original, target] of [objectPattern({}), arrayPattern([])]) {
        expect(target).not.toBe(original);
        expect(original.foo).toBe(7);
        expect(target.foo).toBeUndefined();
    }
});

test("default that overwrites the key of its computed MemberExpression target", () => {
    function objectPattern(source) {
        const target = {};
        let key = "a";
        ({ x: target[key] = ((key = "b"), 7) } = source);
        return [target, key];
    }
    function arrayPattern(source) {
        const target = {};
        let key = "a";
        [target[key] = ((key = "b"), 7)] = source;
        return [target, key];
    }

    expect(objectPattern({ x: 3 })).toEqual([{ a: 3 }, "a"]);
    expect(arrayPattern([3])).toEqual([{ a: 3 }, "a"]);
    expect(objectPattern({})).toEqual([{ a: 7 }, "b"]);
    expect(arrayPattern([])).toEqual([{ a: 7 }, "b"]);
});

test("default that overwrites an argument its MemberExpression target uses", () => {
    function argumentBase(target, source) {
        const original = target;
        ({ x: target.foo = ((target = {}), 7) } = source);
        return [original, target];
    }
    function argumentKey(key, source) {
        const target = {};
        [target[key] = ((key = "b"), 7)] = source;
        return [target, key];
    }

    let [original, target] = argumentBase({}, { x: 3 });
    expect(target).toBe(original);
    expect(original.foo).toBe(3);
    [original, target] = argumentBase({}, {});
    expect(target).not.toBe(original);
    expect(original.foo).toBe(7);
    expect(argumentKey("a", [3])).toEqual([{ a: 3 }, "a"]);
    expect(argumentKey("a", [])).toEqual([{ a: 7 }, "b"]);
});

test("MemberExpression target after an earlier run took the default", () => {
    function run(sources) {
        const targets = [];
        for (const source of sources) {
            let target = {};
            targets.push(target);
            ({ x: target.foo = ((target = {}), 7) } = source);
        }
        return targets.map(target => target.foo);
    }

    expect(run([{}, { x: 3 }, {}, { x: 4 }])).toEqual([7, 3, 7, 4]);
});
