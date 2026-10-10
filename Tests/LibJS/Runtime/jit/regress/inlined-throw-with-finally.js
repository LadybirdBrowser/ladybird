// When an inlined callee throws, the handler and finally block of the caller must see the caller's locals as they
// were, including values compiled code only kept in registers, not stale frame slots.

test("caller locals after an inlined callee throws", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const check = value => {
                if (typeof value !== "number") throw new TypeError(`not a number: ${typeof value}`);
                return value + 1;
            };
            return (value, log) => {
                let before = value * 3;
                let after = 0;
                try {
                    after = check(value) + before;
                    before = -1;
                } catch (e) {
                    log.push(`caught with before=${before} after=${after}`);
                } finally {
                    log.push(`finally with before=${before} after=${after}`);
                }
                return [before, after];
            };
        },
        {
            warmUp: (log, i) => [i, log],
            probes: { string: log => ["x", log], undefined: log => [undefined, log], double: log => [2.5, log] },
        }
    );
    expect(failures).toEqual([]);
});

test("a method that first reads this inside a loop is inlined", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const object = {
                n: 2,
                loop() {
                    for (var i = 0; i < this.n; i++) {}
                    return this.n;
                },
            };
            return n => {
                object.n = n;
                return object.loop();
            };
        },
        { warmUp: (log, i) => [i % 4], probes: { string: () => ["3"], double: () => [2.5] } }
    );
    expect(failures).toEqual([]);
});
