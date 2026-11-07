test("capturing native calls retain their state and guard the target", () => {
    const first = Proxy.revocable({}, {});
    const second = Proxy.revocable({}, {});
    function invoke(revoke) {
        return revoke();
    }
    jit.prepare(invoke);
    for (let i = 0; i < 20; ++i) expect(invoke(first.revoke)).toBeUndefined();
    expect(jit.compile(invoke) || !jit.enabled).toBeTrue();
    gc();
    expect(invoke(first.revoke)).toBeUndefined();
    expect(Reflect.ownKeys(second.proxy)).toEqual([]);
    expect(invoke(second.revoke)).toBeUndefined();
    expect(() => Reflect.ownKeys(second.proxy)).toThrow(TypeError);
    expect(invoke(() => 42)).toBe(42);
});

test("exceptions from capturing native calls unwind inlined callers", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            function strictArguments() {
                "use strict";
                return arguments;
            }
            const thrower = Object.getOwnPropertyDescriptor(strictArguments(), "callee").get;
            function inner(value) {
                thrower(value);
            }
            function middle(value) {
                return inner(value);
            }
            return (value, log) => {
                gc();
                try {
                    return middle(value);
                } catch (error) {
                    log.push(error instanceof TypeError);
                    return value;
                } finally {
                    log.push("finally");
                }
            };
        },
        {
            warmUp: (log, i) => [i, log],
            probes: { object: log => [{ marker: 42 }, log] },
        }
    );
    expect(failures).toEqual([]);
});
