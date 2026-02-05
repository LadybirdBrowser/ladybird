test("length is 2", () => {
    expect(FinalizationRegistry.prototype.register).toHaveLength(2);
});

test("basic functionality", () => {
    var registry = new FinalizationRegistry(() => {});

    var target1 = {};
    var heldValue1 = {};

    registry.register(target1, heldValue1);

    var target2 = {};
    var heldValue2 = {};
    var token1 = {};

    registry.register(target2, heldValue2, token1);

    var target3 = Symbol("target");
    var heldValue3 = {};
    var token2 = Symbol("token");

    registry.register(target3, heldValue3, token2);
});

test("errors", () => {
    var registry = new FinalizationRegistry(() => {});

    expect(() => {
        registry.register(5, {});
    }).toThrowWithMessage(TypeError, "cannot be held weakly");

    expect(() => {
        var a = {};
        registry.register(a, a);
    }).toThrowWithMessage(TypeError, "Target and held value must not be the same");

    expect(() => {
        var a = Symbol();
        registry.register(a, a);
    }).toThrowWithMessage(TypeError, "Target and held value must not be the same");

    expect(() => {
        registry.register({}, {}, 5);
    }).toThrowWithMessage(TypeError, "cannot be held weakly");
});
