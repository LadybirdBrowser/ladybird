// The JIT code of a function that did not run for a while is flushed, and the function runs in the interpreter until
// it gets hot again. flushCode("jit") flushes the JIT code of every function no frame runs, whatever its age.

// NB: The functions the tests declare keep their shared data across the harness's repeated runs of a test, so that each
//     run would add recompiles, which stop the flushes of a function after six. So the body runs once.
function flushingTest(name, body) {
    let ran = false;
    test(name, () => {
        if (ran) return;
        ran = true;
        body();
    });
}

function compileHot(f, warmUp) {
    jit.prepare(f);
    warmUp();
    return jit.compile(f);
}

flushingTest("flushed JIT code compiles again", () => {
    function scale(value) {
        return value * 3 + 1;
    }
    const warmUp = () => {
        for (let i = 0; i < 10; ++i) expect(scale(i)).toBe(i * 3 + 1);
    };
    expect(compileHot(scale, warmUp)).toBe(jit.enabled);
    expect(jit.isCompiled(scale)).toBe(jit.enabled);
    flushCode("jit");
    expect(jit.isCompiled(scale)).toBeFalse();
    expect(scale(5)).toBe(16);
    expect(compileHot(scale, warmUp)).toBe(jit.enabled);
    expect(scale(7)).toBe(22);
});

flushingTest("callers of flushed JIT code keep working", () => {
    function callee(value) {
        let result = value;
        for (let i = 0; i < 3; ++i) result = result * 2 + 1;
        return result;
    }
    function caller(value) {
        return callee(value) + callee(value + 1);
    }
    const warmUp = () => {
        for (let i = 0; i < 10; ++i) expect(caller(i)).toBe(16 * i + 22);
    };
    expect(compileHot(callee, warmUp)).toBe(jit.enabled);
    expect(compileHot(caller, warmUp)).toBe(jit.enabled);
    flushCode("jit");
    expect(jit.isCompiled(callee)).toBeFalse();
    expect(jit.isCompiled(caller)).toBeFalse();
    expect(caller(2)).toBe(54);
    expect(compileHot(caller, warmUp)).toBe(jit.enabled);
    expect(caller(3)).toBe(70);
});

flushingTest("running functions keep their JIT code", () => {
    function running(flush) {
        if (flush) flushCode("jit");
        return jit.isCompiled(running);
    }
    jit.prepare(running);
    for (let i = 0; i < 10; ++i) running(false);
    expect(jit.compile(running)).toBe(jit.enabled);
    // NB: Forced exits can discard the code of the running function on their own.
    const compiled = running(true);
    if (!jit.forcesExits) expect(compiled).toBe(jit.enabled);
});
