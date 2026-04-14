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
