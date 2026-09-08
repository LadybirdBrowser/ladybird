test("global identifier cache is invalidated after lexical shadowing", () => {
    evaluateSource(`
        globalThis.globalCacheShadowValue = 40;
        function updateGlobalCacheShadowValue() {
            return globalCacheShadowValue += 1;
        }
    `);

    expect(evaluateSource("updateGlobalCacheShadowValue();")).toBe(41);
    expect(evaluateSource("updateGlobalCacheShadowValue();")).toBe(42);

    evaluateSource("let globalCacheShadowValue = 100;");
    expect(evaluateSource("updateGlobalCacheShadowValue();")).toBe(101);
    expect(evaluateSource("updateGlobalCacheShadowValue();")).toBe(102);
    expect(globalThis.globalCacheShadowValue).toBe(42);
});
