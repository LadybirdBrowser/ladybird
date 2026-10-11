test("inlined scan exits preserve caller state and empty scans", () => {
    const find = new Function(
        "array",
        "target",
        "for (let i = 0; i < array.length; ++i) if (array[i].pos == target.pos) return array[i]; return false;"
    );
    const caller = new Function(
        "find",
        "array",
        "target",
        "state",
        "++state.before; const result = find(array, target); ++state.after; return result;"
    );
    const position = {};
    const array = [{ pos: {} }, { pos: position }];
    const state = { before: 0, after: 0 };
    jit.prepare(find);
    jit.prepare(caller);
    for (let i = 0; i < 10; ++i) caller(find, array, { pos: position }, state);
    expect(jit.compile(caller)).toBe(jit.enabled);
    state.before = 0;
    state.after = 0;
    expect(caller(find, array, { pos: position }, state)).toBe(array[1]);
    expect(caller(find, [], null, state)).toBeFalse();
    let getterCalls = 0;
    const target = {
        get pos() {
            ++getterCalls;
            return position;
        },
    };
    expect(caller(find, [], target, state)).toBeFalse();
    expect(getterCalls).toBe(0);
    expect(caller(find, array, target, state)).toBe(array[1]);
    expect(getterCalls).toBe(2);
    expect(state.before).toBe(4);
    expect(state.after).toBe(4);
    expect(() => caller(find, array, null, state)).toThrow(TypeError);
    expect(state.before).toBe(5);
    expect(state.after).toBe(4);
});

test("inlined loop hoisting keeps nested caller and induction state on exits", () => {
    const total = new Function(
        "object",
        "count",
        "let sum = 0; for (let i = 0; i < count; ++i) sum = (sum + object.value) | 0; return sum;"
    );
    const middle = new Function("total", "object", "count", "return total(object, count) + 10;");
    const caller = new Function(
        "middle",
        "total",
        "object",
        "count",
        "state",
        "++state.before; return middle(total, object, count) + state.before;"
    );
    const state = { before: 0 };
    jit.prepare(total);
    jit.prepare(middle);
    jit.prepare(caller);
    for (let i = 0; i < 10; ++i) caller(middle, total, { value: 2 }, 3, state);
    expect(jit.compile(caller)).toBe(jit.enabled);
    state.before = 0;
    expect(caller(middle, total, { value: 2 }, 3, state)).toBe(17);
    expect(caller(middle, total, null, 0, state)).toBe(12);
    let getterCalls = 0;
    const object = {
        get value() {
            return ++getterCalls;
        },
    };
    expect(caller(middle, total, object, 3, state)).toBe(19);
    expect(getterCalls).toBe(3);
    expect(state.before).toBe(3);
    expect(caller(middle, total, { value: 2 }, "3", state)).toBe(20);
    expect(state.before).toBe(4);
});
