describe("errors", () => {
    test("cannot spread number in array", () => {
        expect(() => {
            [...1];
        }).toThrowWithMessage(TypeError, "1 is not iterable");
    });

    test("cannot spread object in array", () => {
        expect(() => {
            [...{}];
        }).toThrowWithMessage(TypeError, "[object Object] is not iterable");
    });
});

test("basic functionality", () => {
    expect([1, ...[2, 3], 4]).toEqual([1, 2, 3, 4]);

    let a = [2, 3];
    expect([1, ...a, 4]).toEqual([1, 2, 3, 4]);

    let obj = { a: [2, 3] };
    expect([1, ...obj.a, 4]).toEqual([1, 2, 3, 4]);

    expect([...[], ...[...[1, 2, 3]], 4]).toEqual([1, 2, 3, 4]);
});

test("allows assignment expressions", () => {
    expect("([ ...a = { hello: 'world' } ])").toEval();
    expect("([ ...a += 'hello' ])").toEval();
    expect("([ ...a -= 'hello' ])").toEval();
    expect("([ ...a **= 'hello' ])").toEval();
    expect("([ ...a *= 'hello' ])").toEval();
    expect("([ ...a /= 'hello' ])").toEval();
    expect("([ ...a %= 'hello' ])").toEval();
    expect("([ ...a <<= 'hello' ])").toEval();
    expect("([ ...a >>= 'hello' ])").toEval();
    expect("([ ...a >>>= 'hello' ])").toEval();
    expect("([ ...a &= 'hello' ])").toEval();
    expect("([ ...a ^= 'hello' ])").toEval();
    expect("([ ...a |= 'hello' ])").toEval();
    expect("([ ...a &&= 'hello' ])").toEval();
    expect("([ ...a ||= 'hello' ])").toEval();
    expect("([ ...a ??= 'hello' ])").toEval();
    expect("function* test() { return ([ ...yield a ]); }").toEval();
});
