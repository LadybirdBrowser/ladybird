test("basic functionality", () => {
    expect(Symbol).toHaveProperty("iterator");
    expect(Symbol).toHaveProperty("asyncIterator");
    expect(Symbol).toHaveProperty("match");
    expect(Symbol).toHaveProperty("matchAll");
    expect(Symbol).toHaveProperty("replace");
    expect(Symbol).toHaveProperty("search");
    expect(Symbol).toHaveProperty("split");
    expect(Symbol).toHaveProperty("hasInstance");
    expect(Symbol).toHaveProperty("isConcatSpreadable");
    expect(Symbol).toHaveProperty("unscopables");
    expect(Symbol).toHaveProperty("species");
    expect(Symbol).toHaveProperty("toPrimitive");
    expect(Symbol).toHaveProperty("toStringTag");
    expect(Symbol).toHaveProperty("dispose");
});
