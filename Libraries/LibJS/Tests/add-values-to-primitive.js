test("adding objects", () => {
    expect([] + []).toBe("");
    expect([] + {}).toBe("[object Object]");
    expect({} + {}).toBe("[object Object][object Object]");
    expect({} + []).toBe("[object Object]");
});
