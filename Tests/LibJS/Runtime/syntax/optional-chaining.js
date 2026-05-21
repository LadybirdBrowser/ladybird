test("parse optional-chaining", () => {
    expect(`a?.b`).toEval();
    expect(`a?.4:.5`).toEval();
    expect(`a?.[b]`).toEval();
    expect(`a?.b[b]`).toEval();
    expect(`a?.b(c)`).toEval();
    expect(`a?.b?.(c, d)`).toEval();
    expect(`a?.b?.()`).toEval();
    expect("a?.b``").not.toEval();
    expect("a?.b?.``").not.toEval();
    expect("new Foo?.bar").not.toEval();
    expect("new (Foo?.bar)").toEval();
    expect("(new Foo)?.bar").toEval();
});

test("evaluate optional-chaining", () => {
    for (let nullishObject of [null, undefined]) {
        expect((() => nullishObject?.b)()).toBeUndefined();
    }

    expect(
        (() => {
            let a = {};
            return a?.foo?.bar?.baz;
        })()
    ).toBeUndefined();

    expect(
        (() => {
            let a = { foo: { bar: () => 42 } };
            return `${a?.foo?.bar?.()}-${a?.foo?.baz?.()}`;
        })()
    ).toBe("42-undefined");

    expect(() => {
        let a = { foo: { bar: () => 42 } };
        return a.foo?.baz.nonExistentProperty;
    }).toThrow();
});

test("delete optional-chaining property references", () => {
    let object = { value: 1, computed: 2 };
    expect(delete object?.value).toBeTrue();
    expect(object.value).toBeUndefined();

    expect(delete object?.["computed"]).toBeTrue();
    expect(object.computed).toBeUndefined();

    let keyEvaluations = 0;
    expect(delete undefined?.[keyEvaluations++]).toBeTrue();
    expect(keyEvaluations).toBe(0);

    expect(delete { inner: null }?.inner?.value).toBeTrue();
});
