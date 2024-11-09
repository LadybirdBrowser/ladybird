test("length is 2", () => {
    expect(FinalizationRegistry.prototype.unregister).toHaveLength(1);
});

test("basic functionality", () => {
    var registry = new FinalizationRegistry(() => {});

    var target1 = {};
    var heldValue1 = {};
    var token1 = {};

    registry.register(target1, heldValue1, token1);

    expect(registry.unregister({})).toBe(false);
    expect(registry.unregister(token1)).toBe(true);
    expect(registry.unregister(token1)).toBe(false);

    var target2 = Symbol("target");
    var heldValue2 = Symbol("heldValue");
    var token2 = Symbol("token");

    registry.register(target2, heldValue2, token2);

    expect(registry.unregister(Symbol("token"))).toBe(false);
    expect(registry.unregister(token2)).toBe(true);
    expect(registry.unregister(token2)).toBe(false);
});

test("errors", () => {
    var registry = new FinalizationRegistry(() => {});

    expect(() => {
        registry.unregister(5);
    }).toThrowWithMessage(TypeError, "cannot be held weakly");
});
