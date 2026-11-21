describe("conditions generated directly as control flow", () => {
    test("statements whose test branches at its operators complete with their own values", () => {
        expect(eval("1; if (true && false) 2;")).toBeUndefined();
        expect(eval("1; if (false || !true) 2; else 3;")).toBe(3);
        expect(eval("1; if (true && true) 2;")).toBe(2);
        expect(eval("1; while (false || false) 2;")).toBeUndefined();
        expect(eval("let i = 0; 1; do i++; while (i < 3 && i > 0);")).toBe(2);
    });

    test("nested logical and conditional tests preserve truthiness and value contexts", () => {
        function condition(a, b, c, choose) {
            if (!(choose ? a && b : a || c)) return "false";
            return "true";
        }
        function value(a, b, c, choose) {
            return choose ? a && b : a || c;
        }
        const values = [undefined, null, false, true, 0, -0, NaN, 1, "", "x", 0n, 1n, {}, Symbol()];
        for (const a of values) {
            for (const b of values) {
                for (const choose of [false, true]) {
                    expect(condition(a, b, "fallback", choose)).toBe(
                        value(a, b, "fallback", choose) ? "true" : "false"
                    );
                }
            }
        }
        expect(value("left", "right", "fallback", true)).toBe("right");
        expect(value("left", "right", "fallback", false)).toBe("left");
    });

    test("comparison coercions keep their order, exceptions, and short circuit", () => {
        function check(a, b, c) {
            if (a < b && !(b >= c)) return true;
            return false;
        }
        const log = [];
        const object = (name, number, throws = false) => ({
            valueOf() {
                log.push(name);
                if (throws) throw new Error(name);
                return number;
            },
        });
        expect(check(object("a", 1), object("b", 2), object("c", 3))).toBeTrue();
        expect(log).toEqual(["a", "b", "b", "c"]);
        log.length = 0;
        expect(check(object("a", 3), object("b", 2), object("c", 1, true))).toBeFalse();
        expect(log).toEqual(["a", "b"]);
        log.length = 0;
        expect(() => check(object("a", 1), object("b", 2), object("c", 3, true))).toThrowWithMessage(Error, "c");
        expect(log).toEqual(["a", "b", "b", "c"]);
        expect(check(1, 2, NaN)).toBeTrue();
        expect(check(1n, 2n, 3n)).toBeTrue();
        expect(() => check(Symbol(), 2, 3)).toThrow(TypeError);
    });

    test("assignments used as conditions still store comparison values", () => {
        function assign(flag, a, b) {
            let stored = "untouched";
            if (flag && (stored = a < b)) return [true, stored];
            return [false, stored];
        }
        expect(assign(false, 1, 2)).toEqual([false, "untouched"]);
        expect(assign(true, 1, 2)).toEqual([true, true]);
        expect(assign(true, 2, 1)).toEqual([false, false]);
    });

    test("pending operands survive writes in a conditional test and its arms", () => {
        function testWrite(flag) {
            let x = 10;
            return x + (flag && (x = 20) ? x : 1);
        }
        function armWrite(flag, branch) {
            let x = 10;
            return x + (flag || branch ? (x = 20) : (x = 30));
        }
        expect(testWrite(false)).toBe(11);
        expect(testWrite(true)).toBe(30);
        expect(armWrite(false, false)).toBe(40);
        expect(armWrite(false, true)).toBe(30);
        expect(armWrite(true, false)).toBe(30);
    });

    test("conditional arms and comma prefixes only run on their selected paths", () => {
        function check(select, left, right, log) {
            if ((log.push("prefix"), select ? left && log.push("left") : right || log.push("right"))) return true;
            return false;
        }
        const log = [];
        expect(check(true, 0, 0, log)).toBeFalse();
        expect(log).toEqual(["prefix"]);
        log.length = 0;
        expect(check(true, 1, 0, log)).toBeTrue();
        expect(log).toEqual(["prefix", "left"]);
        log.length = 0;
        expect(check(false, 1, 0, log)).toBeTrue();
        expect(log).toEqual(["prefix", "right"]);
    });

    test("nullish expressions retain their values inside predicate ternaries", () => {
        function check(select, left, right) {
            if (select ? (left ?? right) : !(left ?? right)) return true;
            return false;
        }
        function value(first, second, left, right) {
            return first && second ? (left ?? right) : left || right;
        }
        expect(check(true, 0, 7)).toBeFalse();
        expect(check(true, null, 7)).toBeTrue();
        expect(check(false, undefined, 0)).toBeTrue();
        expect(check(false, false, 7)).toBeTrue();
        expect(value(true, true, 0, 7)).toBe(0);
        expect(value(true, false, 0, 7)).toBe(7);
        expect(value(true, true, null, "fallback")).toBe("fallback");
        let calls = 0;
        function shortCircuit(left) {
            if (left ?? (++calls ? true : false)) return true;
            return false;
        }
        expect(shortCircuit(0)).toBeFalse();
        expect(calls).toBe(0);
        expect(shortCircuit(null)).toBeTrue();
        expect(calls).toBe(1);
    });

    test("while and do-while tests preserve side effects, continue, break, and finally", () => {
        function run(useWhile, limit) {
            let i = 0;
            const log = [];
            function check() {
                log.push(`test:${i}`);
                return i < limit && i !== 4;
            }
            if (useWhile) {
                outer: while (check()) {
                    try {
                        ++i;
                        if (i === 2) continue outer;
                        if (i === 3 && limit > 5) break outer;
                        log.push(`body:${i}`);
                    } finally {
                        log.push(`finally:${i}`);
                    }
                }
            } else {
                outer: do {
                    try {
                        ++i;
                        if (i === 2) continue outer;
                        if (i === 3 && limit > 5) break outer;
                        log.push(`body:${i}`);
                    } finally {
                        log.push(`finally:${i}`);
                    }
                } while (check());
            }
            return log;
        }
        expect(run(true, 0)).toEqual(["test:0"]);
        expect(run(false, 0)).toEqual(["body:1", "finally:1", "test:1"]);
        expect(run(true, 8)).toEqual(["test:0", "body:1", "finally:1", "test:1", "finally:2", "test:2", "finally:3"]);
        expect(run(false, 3)).toEqual([
            "body:1",
            "finally:1",
            "test:1",
            "finally:2",
            "test:2",
            "body:3",
            "finally:3",
            "test:3",
        ]);
    });

    test("nested loop conditions and lexical environments retain their own targets", () => {
        function collect() {
            let i = 0;
            const closures = [];
            while (i < 3) {
                const value = i++;
                let j = 0;
                do {
                    ++j;
                    if (j < 2) continue;
                    closures.push(() => [value, j]);
                } while (j < 2 || false);
            }
            return closures.map(callback => callback());
        }
        expect(collect()).toEqual([
            [0, 2],
            [1, 2],
            [2, 2],
        ]);
    });

    test("statement completion values and parameter expression heads remain valid", () => {
        expect(eval("42; if (false && true) 7;")).toBeUndefined();
        expect(eval("42; if (!(false || false)) 7;")).toBe(7);
        expect(eval("42; while (false) 7;")).toBeUndefined();
        expect(eval("let i=0; do { ++i; 7; } while (i<2 && true);")).toBe(7);
        function defaults(a = true && (false ? 3 : 4), b = false || 5) {
            if (!(a < b) || (a > b ? false : true)) return [a, b];
            return [];
        }
        expect(defaults()).toEqual([4, 5]);
    });
});
