test("return from try followed by finally with function call inside", () => {
    let value = (() => {
        try {
            return 1;
        } finally {
            (() => {})();
        }
    })();

    expect(value).toBe(1);
});

test("return from outer finally with nested unwind contexts", () => {
    let value = (() => {
        try {
            try {
                return 1;
            } finally {
            }
        } finally {
            return 2;
        }
    })();

    expect(value).toBe(2);
});

test("restore exception after generator yield in finally", () => {
    let generator = (function* () {
        try {
            throw new Error("foo");
        } finally {
            yield 42;
        }
    })();

    expect(generator.next().value).toBe(42);
    expect(() => generator.next()).toThrowWithMessage(Error, "foo");
    expect(generator.next().done).toBe(true);
});

test("yield, then return from finally", () => {
    let test = [];
    let generator = (function* () {
        try {
            yield 1;
            test.push(1);
        } finally {
            test.push(2);
            return 2;
        }
        expect.fail("unreachable");
    })();

    let result = generator.next();
    expect(result.value).toBe(1);
    expect(result.done).toBe(false);
    result = generator.next();
    expect(result.value).toBe(2);
    expect(result.done).toBe(true);
    expect(test).toEqual([1, 2]);
});

test("return from async through finally", () => {
    let test = 0;
    let result = (async function () {
        try {
            return { y: 5 };
        } finally {
            test = 42;
        }
        expect.fail("unreachable");
    })();

    expect(result).toBeInstanceOf(Promise);
    expect(test).toBe(42);
    result.then(value => {
        expect(value).toEqual({ y: 5 });
    });
});

describe("every way out of a scope restores the environment for finally", () => {
    const yieldExit = { statement: "yield", expression: "yield" };
    const throwExit = { statement: "thrower()", expression: "thrower()" };
    const exits = {
        "generator return": {
            ...yieldExit,
            run: generator => {
                generator.next();
                expect(generator.return("r")).toEqual({ value: "r", done: true });
            },
        },
        "generator throw": {
            ...yieldExit,
            run: generator => {
                generator.next();
                expect(() => generator.throw(new Error())).toThrow(Error);
            },
        },
        throw: {
            ...throwExit,
            run: generator => expect(() => generator.next()).toThrow(Error),
        },
        return: {
            statement: "return 'r'",
            run: generator => expect(generator.next()).toEqual({ value: "r", done: true }),
        },
        break: {
            statement: "break outer",
            run: generator => expect(generator.next()).toEqual({ value: undefined, done: true }),
        },
        continue: {
            statement: "continue outer",
            run: generator => expect(generator.next()).toEqual({ value: undefined, done: true }),
        },
    };

    const scopes = {
        "a block": "{ let t = 1; capture(() => t); STATEMENT; }",
        "a for loop body": "for (let i = 0; i < 1; i++) { capture(() => i); STATEMENT; }",
        "a for-of loop body": "for (const x of [1]) { capture(() => x); STATEMENT; }",
        "a for-in loop body": "for (const x in { p: 1 }) { capture(() => x); STATEMENT; }",
        "a catch clause": "try { throw 1; } catch (e) { capture(() => e); STATEMENT; }",
        "a try block with a finally clause": "try { STATEMENT; } finally {}",
        "a switch case": "switch (0) { case 0: let s = 1; capture(() => s); STATEMENT; }",
        "a with statement": "with ({}) { STATEMENT; }",
        "a for-of loop head": "for (const x of [EXPRESSION]) { capture(() => x); }",
        "a for-in loop variable default": "for (const { x = EXPRESSION } in { p: 1 }) { capture(() => x); }",
        "an array destructuring default": "let [y = EXPRESSION] = []; capture(() => y);",
        "a class heritage": "class C extends (EXPRESSION, Object) {}",
        "a class computed key": "class C { [EXPRESSION] = 1; }",
        "an anonymous class computed key": "(class { [EXPRESSION] = 1; });",
        "a class computed key after private names": "class C { #p = 1; [EXPRESSION] = 1; }",
    };

    for (const [scopeName, scope] of Object.entries(scopes)) {
        for (const [exitName, exit] of Object.entries(exits)) {
            const code = scope.includes("STATEMENT")
                ? exit.statement && scope.replace("STATEMENT", exit.statement)
                : exit.expression && scope.replace("EXPRESSION", exit.expression);
            if (!code) continue;

            for (const inBlock of [false, true]) {
                const body = inBlock ? `{ let w = 1; capture(() => w); ${code} }` : code;
                test(`${exitName} from ${scopeName}${inBlock ? " in a block" : ""}`, () => {
                    const makeGenerator = new Function(
                        "capture",
                        "seen",
                        "thrower",
                        `return function* () {
                            let a = "a";
                            let b = "b";
                            capture(() => a + b);
                            outer: for (let iteration = 0; iteration < 1; iteration++) {
                                try {
                                    ${body}
                                } finally {
                                    seen.push(b);
                                    b = "assigned";
                                }
                            }
                        };`
                    );
                    const captured = [];
                    const seen = [];
                    const thrower = () => {
                        throw new Error();
                    };
                    exit.run(makeGenerator(binding => captured.push(binding), seen, thrower)());
                    expect(seen).toEqual(["b"]);
                    expect(captured[0]()).toBe("aassigned");
                });
            }
        }
    }
});
