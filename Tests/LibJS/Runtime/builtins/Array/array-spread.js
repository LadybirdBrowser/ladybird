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

test("elisions after spread remain holes", () => {
    let array = [...[], ,];
    expect(array).toHaveLength(1);
    expect(array.hasOwnProperty(0)).toBeFalse();
    expect(0 in array).toBeFalse();
    expect(array[0]).toBeUndefined();
    expect(String(array[0])).toBe("undefined");

    array = [1, ...[], ,];
    expect(array).toHaveLength(2);
    expect(array.hasOwnProperty(0)).toBeTrue();
    expect(array.hasOwnProperty(1)).toBeFalse();
    expect(1 in array).toBeFalse();
    expect(array[1]).toBeUndefined();
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
