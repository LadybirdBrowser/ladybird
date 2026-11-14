// Functions that only read arguments.length and arguments[i].

function repeat(callback) {
    for (let i = 0; i < 5; ++i) callback();
}

describe("arguments.length", () => {
    test("counts passed arguments, not parameters", () => {
        function sloppy(a, b) {
            return arguments.length;
        }
        function strict(a, b) {
            "use strict";
            return arguments.length;
        }
        function nonSimple(a = 1, b) {
            return arguments.length;
        }
        function noParameters() {
            return arguments.length;
        }
        repeat(() => {
            for (const f of [sloppy, strict, nonSimple, noParameters]) {
                expect(f()).toBe(0);
                expect(f(1)).toBe(1);
                expect(f(1, 2)).toBe(2);
                expect(f(1, 2, 3, 4)).toBe(4);
                expect(f(undefined)).toBe(1);
            }
        });
    });

    test("through call, apply, bind and spread", () => {
        function f(a) {
            return arguments.length;
        }
        repeat(() => {
            expect(f.call(null, 1, 2)).toBe(2);
            expect(f.apply(null, [1, 2, 3])).toBe(3);
            expect(f.bind(null, 1)(2, 3)).toBe(3);
            expect(f(...[1, 2, 3, 4, 5])).toBe(5);
            expect(new f(1, 2) instanceof f).toBeTrue();
        });
    });

    test("in parameter expressions", () => {
        function f(a, b = arguments.length) {
            return b;
        }
        repeat(() => {
            expect(f()).toBe(0);
            expect(f(1)).toBe(1);
            expect(f(1, undefined, 3)).toBe(3);
        });
    });
});

describe("arguments[i]", () => {
    test("reads passed arguments, including extra ones", () => {
        function sloppy(a) {
            return [arguments[0], arguments[1], arguments[2]];
        }
        function strict(a) {
            "use strict";
            return [arguments[0], arguments[1], arguments[2]];
        }
        repeat(() => {
            for (const f of [sloppy, strict]) {
                expect(f()).toEqual([undefined, undefined, undefined]);
                expect(f(1)).toEqual([1, undefined, undefined]);
                expect(f(1, 2, 3)).toEqual([1, 2, 3]);
                expect(f.apply(null, ["a", "b", "c", "d"])).toEqual(["a", "b", "c"]);
            }
        });
    });

    test("a loop over all arguments", () => {
        function sum() {
            let s = 0;
            for (let i = 0; i < arguments.length; ++i) s += arguments[i];
            return s;
        }
        function strictSum(a, b) {
            "use strict";
            let s = 0;
            for (let i = 0; i < arguments.length; ++i) s += arguments[i];
            return s;
        }
        repeat(() => {
            expect(sum()).toBe(0);
            expect(sum(1, 2, 3)).toBe(6);
            expect(sum(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)).toBe(55);
            expect(strictSum(1)).toBe(1);
            expect(strictSum(1, 2, 3, 4)).toBe(10);
        });
    });

    test("mapped arguments see the current value of the parameter", () => {
        function f(a, b) {
            a = "changed";
            b++;
            return [arguments[0], arguments[1], arguments[2]];
        }
        function g(a) {
            const before = arguments[0];
            a = 2;
            return [before, arguments[0]];
        }
        repeat(() => {
            expect(f(1, 2, 3)).toEqual(["changed", 3, 3]);
            expect(g(1)).toEqual([1, 2]);
        });
    });

    test("parameters without an argument are not mapped", () => {
        function f(a, b) {
            a = 1;
            b = 2;
            return [arguments.length, arguments[0], arguments[1]];
        }
        repeat(() => {
            expect(f()).toEqual([0, undefined, undefined]);
            expect(f("x")).toEqual([1, 1, undefined]);
        });
    });

    test("duplicate parameters map the last one of the name", () => {
        function f(a, a) {
            a = "changed";
            return [arguments[0], arguments[1]];
        }
        repeat(() => {
            expect(f(1, 2)).toEqual([1, "changed"]);
        });
    });

    test("mapped parameters that a closure writes", () => {
        function f(a) {
            const write = () => {
                a = "written";
            };
            write();
            return arguments[0];
        }
        function g(a) {
            const read = () => a;
            a = "changed";
            return [arguments[0], read()];
        }
        repeat(() => {
            expect(f(1)).toBe("written");
            expect(g(1)).toEqual(["changed", "changed"]);
        });
    });

    test("unmapped arguments keep the passed value", () => {
        function strict(a, b) {
            "use strict";
            a = "changed";
            b++;
            return [arguments[0], arguments[1]];
        }
        function strictVar(a) {
            "use strict";
            var a = "changed";
            return arguments[0];
        }
        function strictUpdate(a) {
            "use strict";
            for (a of ["changed"]);
            return arguments[0];
        }
        function strictDestructuring(a) {
            "use strict";
            [a] = ["changed"];
            return arguments[0];
        }
        function defaults(a = "default", b) {
            return [arguments[0], arguments[1]];
        }
        function rest(a, ...b) {
            return [arguments[0], arguments[1], arguments[2]];
        }
        function pattern({ x }, b) {
            b = "changed";
            return [arguments[0].x, arguments[1]];
        }
        repeat(() => {
            expect(strict(1, 2)).toEqual([1, 2]);
            expect(strictVar(1)).toBe(1);
            expect(strictUpdate(1)).toBe(1);
            expect(strictDestructuring(1)).toBe(1);
            expect(defaults()).toEqual([undefined, undefined]);
            expect(defaults(undefined, 2)).toEqual([undefined, 2]);
            expect(rest(1, 2, 3)).toEqual([1, 2, 3]);
            expect(pattern({ x: 1 }, 2)).toEqual([1, 2]);
        });
    });

    test("keys that are not passed argument indices", () => {
        function sloppy(a) {
            return [
                arguments[-1],
                arguments[1],
                arguments[1.5],
                arguments["0"],
                arguments["00"],
                arguments["length"],
                arguments["callee"] === sloppy,
                arguments[Symbol.iterator] === Array.prototype.values,
                arguments["toString"] === Object.prototype.toString,
            ];
        }
        function strict(a) {
            "use strict";
            return [
                arguments[-1],
                arguments[1],
                arguments[1.5],
                arguments["0"],
                arguments["00"],
                arguments["length"],
                arguments[Symbol.iterator] === Array.prototype.values,
                arguments["toString"] === Object.prototype.toString,
            ];
        }
        function strictCallee() {
            "use strict";
            return arguments["callee"];
        }
        function nonSimpleCallee(a = 1) {
            return arguments["callee"];
        }
        repeat(() => {
            expect(sloppy("x")).toEqual([undefined, undefined, undefined, "x", undefined, 1, true, true, true]);
            expect(strict("x")).toEqual([undefined, undefined, undefined, "x", undefined, 1, true, true]);
            expect(() => strictCallee()).toThrow(TypeError);
            expect(() => nonSimpleCallee()).toThrow(TypeError);
        });
    });

    test("keys that are numbers but not int32", () => {
        function f(a, b) {
            return [arguments[2 / 2], arguments[0.5 * 2], arguments[-0], arguments[2 ** 32]];
        }
        repeat(() => {
            expect(f("a", "b")).toEqual(["b", "b", "a", undefined]);
        });
    });

    test("keys convert to property keys once", () => {
        function f(a) {
            return arguments[key];
        }
        function strict(a) {
            "use strict";
            return arguments[key];
        }
        let conversions = 0;
        const key = {
            toString() {
                ++conversions;
                return "0";
            },
        };
        repeat(() => {
            conversions = 0;
            expect(f("x")).toBe("x");
            expect(conversions).toBe(1);
            conversions = 0;
            expect(strict("y")).toBe("y");
            expect(conversions).toBe(1);
        });
        const throwing = {
            toString() {
                throw new Error("nope");
            },
        };
        function g() {
            return arguments[throwing];
        }
        expect(() => g(1)).toThrowWithMessage(Error, "nope");
    });

    test("missing indices look in the prototype chain", () => {
        function sloppy(a) {
            return [arguments[0], arguments[1], arguments[5], arguments["polluted"]];
        }
        function strict(a) {
            "use strict";
            return [arguments[0], arguments[1], arguments[5], arguments["polluted"]];
        }
        let getterCalls = 0;
        Object.prototype[1] = "one";
        Object.defineProperty(Object.prototype, 5, {
            get() {
                ++getterCalls;
                return "five";
            },
            configurable: true,
        });
        Object.prototype.polluted = "polluted";
        try {
            repeat(() => {
                for (const f of [sloppy, strict]) {
                    getterCalls = 0;
                    expect(f()).toEqual([undefined, "one", "five", "polluted"]);
                    expect(f("a", "b")).toEqual(["a", "b", "five", "polluted"]);
                    expect(f(1, 2, 3, 4, 5, 6)).toEqual([1, 2, 6, "polluted"]);
                    expect(getterCalls).toBe(2);
                }
            });
        } finally {
            delete Object.prototype[1];
            delete Object.prototype[5];
            delete Object.prototype.polluted;
        }
    });

    test("missing keys read Object.prototype as it is at each read", () => {
        function sloppy(a) {
            return arguments[1];
        }
        function strict(a) {
            "use strict";
            return arguments[1];
        }
        function byKey(a, key) {
            return arguments[key];
        }
        function strictByKey(a, key) {
            "use strict";
            return arguments[key];
        }
        const symbol = Symbol("polluted");
        const other = Symbol("other");
        function check(expected) {
            for (let i = 0; i < 200; ++i) {
                expect(sloppy("a")).toBe(expected);
                expect(strict("a")).toBe(expected);
            }
        }
        check(undefined);
        Object.prototype[1] = "one";
        try {
            check("one");
            expect(sloppy("a", "b")).toBe("b");
        } finally {
            delete Object.prototype[1];
        }
        check(undefined);

        let receiverChecks = 0;
        Object.defineProperty(Object.prototype, 1, {
            get() {
                // NB: Index 1 of an array would hit this getter too, so this checks its receiver in place.
                if (
                    Object.prototype.toString.call(this) === "[object Arguments]" &&
                    this.length === 1 &&
                    this[0] === "a"
                )
                    ++receiverChecks;
                return "getter";
            },
            configurable: true,
        });
        try {
            check("getter");
            expect(receiverChecks).toBe(400);
        } finally {
            delete Object.prototype[1];
        }
        check(undefined);

        for (let i = 0; i < 200; ++i) {
            for (const f of [byKey, strictByKey]) {
                expect(f("a", "missing")).toBeUndefined();
                expect(f("a", symbol)).toBeUndefined();
                expect(f("a", other)).toBeUndefined();
                expect(f("a", "length")).toBe(2);
                expect(f("a", Symbol.iterator)).toBe(Array.prototype.values);
                expect(f("a", "hasOwnProperty")).toBe(Object.prototype.hasOwnProperty);
                expect(f("a", "__proto__")).toBe(Object.prototype);
                expect(f("a", 2)).toBeUndefined();
                expect(f("a", "1")).toBe("1");
            }
            expect(byKey("a", "callee")).toBe(byKey);
            expect(() => strictByKey("a", "callee")).toThrow(TypeError);
        }
        const pollute = (key, value) =>
            Object.defineProperty(Object.prototype, key, { value, writable: true, configurable: true });
        pollute("missing", "string");
        pollute(symbol, "symbol");
        pollute("callee", "polluted callee");
        pollute("length", "polluted length");
        try {
            for (const f of [byKey, strictByKey]) {
                expect(f("a", "missing")).toBe("string");
                expect(f("a", symbol)).toBe("symbol");
                expect(f("a", other)).toBeUndefined();
                expect(f("a", "length")).toBe(2);
            }
            expect(byKey("a", "callee")).toBe(byKey);
            expect(() => strictByKey("a", "callee")).toThrow(TypeError);
        } finally {
            delete Object.prototype.missing;
            delete Object.prototype[symbol];
            delete Object.prototype.callee;
            delete Object.prototype.length;
        }
        for (const f of [byKey, strictByKey]) {
            expect(f("a", "missing")).toBeUndefined();
            expect(f("a", symbol)).toBeUndefined();
        }
    });

    test("an index that one call passes and another does not", () => {
        function opt(a) {
            return arguments.length > 1 ? arguments[1] : a;
        }
        function always(a) {
            return arguments[1];
        }
        function strictAlways(a) {
            "use strict";
            return arguments[1];
        }
        function run(round) {
            let result = [];
            for (let i = 0; i < 2000; ++i) {
                if (i === 1000 && round === 1) Object.prototype[1] = "polluted";
                result.push(opt(i), opt(i, "b"), always(i), always(i, "b"), strictAlways(i), strictAlways(i, "b"));
            }
            return result;
        }
        function check(result, pollutedFrom) {
            for (let i = 0; i < 2000; ++i) {
                const missing = i >= pollutedFrom ? "polluted" : undefined;
                expect(result.slice(i * 6, i * 6 + 6)).toEqual([i, "b", missing, "b", missing, "b"]);
            }
        }
        check(run(0), Infinity);
        try {
            check(run(1), 1000);
        } finally {
            delete Object.prototype[1];
        }
        check(run(2), Infinity);
    });

    test("calling a method of an argument", () => {
        function f(a) {
            return arguments[0].toUpperCase() + arguments.length.toString();
        }
        repeat(() => {
            expect(f("x", 2)).toBe("X2");
        });
    });
});

describe("uses that need the arguments object", () => {
    test("calling arguments[i] passes the arguments object as this", () => {
        function f(callback) {
            return arguments[0]();
        }
        function strict(callback) {
            "use strict";
            return arguments[0]();
        }
        repeat(() => {
            for (const g of [f, strict]) {
                const self = g(function () {
                    return this;
                });
                expect(Object.prototype.toString.call(self)).toBe("[object Arguments]");
            }
        });
    });

    test("calling arguments[i] in other ways passes the arguments object as this", () => {
        function self() {
            return this;
        }
        function tagged(callback) {
            return arguments[0]`x`;
        }
        function optional(callback) {
            return arguments[0]?.();
        }
        function parenthesized(callback) {
            // prettier-ignore
            return (arguments[0])();
        }
        function optionalMember(callback) {
            return arguments?.[0]();
        }
        function strictTagged(callback) {
            "use strict";
            return [arguments[0]`x`, arguments.length];
        }
        function deleted(a) {
            // prettier-ignore
            delete (arguments[0]);
            return [arguments[0], arguments.length];
        }
        repeat(() => {
            for (const f of [tagged, optional, parenthesized, optionalMember]) {
                expect(Object.prototype.toString.call(f(self))).toBe("[object Arguments]");
            }
            const [result, length] = strictTagged(self);
            expect(Object.prototype.toString.call(result)).toBe("[object Arguments]");
            expect(length).toBe(1);
            expect(deleted(1, 2)).toEqual([undefined, 2]);
        });
    });

    test("writing arguments[i] writes the mapped parameter", () => {
        function f(a) {
            arguments[0] = "written";
            return [a, arguments[0], arguments.length];
        }
        function update(a) {
            arguments[0]++;
            return a;
        }
        function destructure(a) {
            [arguments[0]] = ["written"];
            return a;
        }
        function strict(a) {
            "use strict";
            arguments[0] = "written";
            return [a, arguments[0]];
        }
        repeat(() => {
            expect(f(1)).toEqual(["written", "written", 1]);
            expect(update(1)).toBe(2);
            expect(destructure(1)).toBe("written");
            expect(strict(1)).toEqual([1, "written"]);
        });
    });

    test("writing and deleting arguments.length", () => {
        function f(a) {
            arguments.length = 5;
            return arguments.length;
        }
        function g(a) {
            delete arguments.length;
            return arguments.length;
        }
        function h(a) {
            delete arguments[0];
            return [a, arguments[0]];
        }
        repeat(() => {
            expect(f(1)).toBe(5);
            expect(g(1)).toBeUndefined();
            expect(h(1)).toEqual([1, undefined]);
        });
    });

    test("other uses of the arguments object", () => {
        function bare(a) {
            return [arguments.length, arguments[0], arguments];
        }
        function property(a) {
            arguments.foo = "foo";
            return [arguments.foo, arguments.length];
        }
        function inArrow(a) {
            const read = () => arguments[0];
            return [arguments.length, read()];
        }
        function withEval(a) {
            return [arguments[0], eval("arguments.length")];
        }
        function optional(a) {
            return [arguments?.length, arguments?.[0]];
        }
        function parenthesized(a) {
            return [arguments.length, arguments[0]];
        }
        function shorthand(a) {
            const first = { arguments }.arguments;
            const second = { arguments }.arguments;
            a = "changed";
            return [first === second, first[0], first.callee === shorthand, arguments[0], arguments.length];
        }
        function afterArrow(a) {
            const count = arguments.length;
            const add = x => x + count;
            return [add(1), arguments[0]];
        }
        repeat(() => {
            expect(shorthand(1, 2)).toEqual([true, "changed", true, "changed", 2]);
            expect(afterArrow(1, 2)).toEqual([3, 1]);
            const result = bare(1, 2);
            expect(result[0]).toBe(2);
            expect(result[1]).toBe(1);
            expect(result[2].length).toBe(2);
            expect(property(1)).toEqual(["foo", 1]);
            expect(inArrow(1, 2)).toEqual([2, 1]);
            expect(withEval(1, 2)).toEqual([1, 2]);
            expect(optional(1, 2)).toEqual([2, 1]);
            expect(parenthesized(1, 2)).toEqual([2, 1]);
        });
    });

    test("arguments that is no arguments object", () => {
        function parameter(arguments) {
            return [arguments.length, arguments[0]];
        }
        function variable(a) {
            var arguments = ["x"];
            return [arguments.length, arguments[0]];
        }
        function lexical(a) {
            let arguments = ["x", "y"];
            return [arguments.length, arguments[0]];
        }
        function block(a) {
            {
                let arguments = ["x", "y", "z"];
                if (a) return [arguments.length, arguments[0]];
            }
            return [arguments.length, arguments[0]];
        }
        function withStatement(a) {
            with ({ arguments: ["w"] }) {
                return [arguments.length, arguments[0]];
            }
        }
        repeat(() => {
            expect(parameter("abc")).toEqual([3, "a"]);
            expect(variable(1, 2)).toEqual([1, "x"]);
            expect(lexical(1)).toEqual([2, "x"]);
            expect(block(1)).toEqual([3, "x"]);
            expect(block(0, 2)).toEqual([2, 0]);
            expect(withStatement(1, 2)).toEqual([1, "w"]);
        });
    });

    test("a shorthand property in a pattern writes arguments", () => {
        function shorthand(a) {
            ({ arguments } = { arguments: ["replaced"] });
            return [arguments[0], arguments.length];
        }
        function withDefault(a) {
            ({ arguments = ["default", "value"] } = {});
            return [arguments[0], arguments.length];
        }
        repeat(() => {
            expect(shorthand(1, 2, 3)).toEqual(["replaced", 1]);
            expect(withDefault(1, 2, 3)).toEqual(["default", 2]);
            expect(Object.hasOwn(globalThis, "arguments")).toBeFalse();
        });
    });

    test("a function in a block of the name arguments replaces the arguments object", () => {
        function f(a) {
            const before = [arguments.length, arguments[0]];
            {
                function arguments(x) {}
            }
            return [before, arguments.length];
        }
        repeat(() => {
            expect(f(1, 2)).toEqual([[2, 1], 1]);
        });
    });

    test("a var of the name arguments keeps the arguments object", () => {
        function f(a) {
            var arguments;
            return [arguments.length, arguments[0]];
        }
        repeat(() => {
            expect(f(1, 2)).toEqual([2, 1]);
        });
    });
});

describe("generators and async functions", () => {
    test("generators read arguments after resuming", () => {
        function* sloppy(a) {
            yield arguments.length;
            a = "changed";
            yield arguments[0];
            yield arguments[1];
        }
        function* strict(a) {
            "use strict";
            yield arguments.length;
            a = "changed";
            yield arguments[0];
            yield arguments[1];
        }
        repeat(() => {
            expect([...sloppy(1, 2)]).toEqual([2, "changed", 2]);
            expect([...strict(1, 2)]).toEqual([2, 1, 2]);
        });
    });

    test("async functions read arguments after await", async () => {
        async function f(a) {
            await null;
            a = "changed";
            return [arguments.length, arguments[0], arguments[1]];
        }
        expect(await f(1, 2)).toEqual([2, "changed", 2]);
    });
});

describe("nested functions", () => {
    test("inner functions have their own arguments", () => {
        function outer(a) {
            function inner(b) {
                return [arguments.length, arguments[0]];
            }
            return [arguments.length, arguments[0], inner("x", "y", "z")];
        }
        repeat(() => {
            expect(outer(1, 2)).toEqual([2, 1, [3, "x"]]);
        });
    });

    test("called from many places", () => {
        function first(a, b) {
            return arguments.length + (arguments[1] ?? 0);
        }
        function caller() {
            let total = 0;
            for (let i = 0; i < 20; ++i) total += first(i) + first(i, i) + first(i, i, i);
            return total;
        }
        repeat(() => {
            expect(caller()).toBe(500);
        });
    });
});

describe("functions inside functions that use the arguments object", () => {
    test("a use of arguments before or after an inner function keeps the object", () => {
        function functionExpression(a) {
            var o = arguments;
            var g = function () {};
            a = 50;
            return [o[0], arguments[0], arguments.length];
        }
        function functionExpressionFirst(a) {
            var g = function () {};
            var o = arguments;
            a = 50;
            return [o[0], arguments[0], arguments.length];
        }
        function arrow(a) {
            var o = arguments;
            var g = () => {};
            a = 50;
            return [o[0], arguments[0], arguments.length];
        }
        function arrowFirst(a) {
            var g = () => 1;
            var o = arguments;
            a = 50;
            return [o[0], arguments[0], arguments.length];
        }
        function classExpression(a) {
            var o = arguments;
            var C = class {
                m() {}
                static {}
                x = 1;
            };
            a = 50;
            return [o[0], arguments[0], arguments.length];
        }
        function classExpressionFirst(a) {
            var C = class {
                m() {}
                static {}
                x = 1;
            };
            var o = arguments;
            a = 50;
            return [o[0], arguments[0], arguments.length];
        }
        function method(a) {
            var o = arguments;
            var p = { m() {}, get x() {} };
            a = 50;
            return [o[0], arguments[0], arguments.length];
        }
        function methodFirst(a) {
            var p = { m() {}, get x() {} };
            var o = arguments;
            a = 50;
            return [o[0], arguments[0], arguments.length];
        }
        function indexedByFunction(a) {
            var o = arguments;
            var v = arguments[function () {}];
            a = 50;
            return [o[0], arguments[0], arguments.length];
        }
        repeat(() => {
            for (const f of [
                functionExpression,
                functionExpressionFirst,
                arrow,
                arrowFirst,
                classExpression,
                classExpressionFirst,
                method,
                methodFirst,
                indexedByFunction,
            ]) {
                expect(f(1, 2)).toEqual([50, 50, 2]);
            }
        });
    });

    test("a function as the key of a read of a variable named arguments", () => {
        expect(new Function("var arguments = {}; return arguments[function () {}];")()).toBeUndefined();
        expect(eval("var arguments = { x: 1 }; arguments[function () {}];")).toBeUndefined();
        function sloppy(a) {
            var v = arguments[function () {}];
            return [v, arguments[0]];
        }
        expect(sloppy(1)).toEqual([undefined, 1]);
    });
});

describe("compiled functions", () => {
    test("parameter written before a branch, then read through arguments[i]", () => {
        let calls = 0;
        function sideEffect() {
            ++calls;
        }
        const object = {
            method(a, flag) {
                a = 7;
                if (flag) sideEffect();
                return arguments[0];
            },
        };
        function plain(a, flag) {
            a = 7;
            if (flag) sideEffect();
            return arguments[0];
        }
        let wrong = 0;
        for (let i = 0; i < 20000; ++i) {
            if (object.method(1, i & 1) !== 7) ++wrong;
            if (plain(1, i & 1) !== 7) ++wrong;
        }
        expect(wrong).toBe(0);
        expect(calls).toBe(20000);
    });
});
