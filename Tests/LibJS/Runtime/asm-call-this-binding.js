test("non-strict asm Call keeps object this binding metadata alive", () => {
    let receiver = {
        value: 41,
        add(delta) {
            return this.value + delta;
        },
    };

    expect(receiver.add(1)).toBe(42);
    expect(receiver.add(2)).toBe(43);
});

test("non-strict asm Call keeps global this binding metadata alive", () => {
    let old_value = globalThis.__asm_call_answer;
    globalThis.__asm_call_answer = 100;

    function add(delta) {
        return this.__asm_call_answer + delta;
    }

    expect(add(23)).toBe(123);

    if (old_value === undefined) delete globalThis.__asm_call_answer;
    else globalThis.__asm_call_answer = old_value;
});

test("asm Call fast-paths native method calls", () => {
    let map = new Map();
    let returned;

    for (let i = 0; i < 100; i++) returned = map.set(i, i + 1);

    expect(returned).toBe(map);
    expect(map.size).toBe(100);
    expect(map.get(41)).toBe(42);
});

test("asm Call unwinds raw native exceptions", () => {
    let getter = Map.prototype.get;

    for (let i = 0; i < 10; i++) expect(() => getter(1)).toThrow(TypeError);
});

test("asm Call lets JS try/catch observe direct raw native throws", () => {
    let caught = false;

    try {
        Object.defineProperty(null, "x", {});
    } catch (error) {
        caught = error instanceof TypeError;
    }

    expect(caught).toBeTrue();
});

test("asm Call lets JS try/catch observe Reflect.construct throws", () => {
    function isConstructor(value) {
        try {
            Reflect.construct(function () {}, [], value);
        } catch {
            return false;
        }
        return true;
    }

    expect(isConstructor(Object.defineProperty)).toBeFalse();
});
