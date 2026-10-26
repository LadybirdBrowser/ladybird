test("object binding targets are resolved before getters and default initializers", () => {
    const log = [];
    const scope = new Proxy(
        {},
        {
            has(_, name) {
                log.push(`binding:${name}`);
                return false;
            },
        }
    );
    const sourceKey = {
        toString() {
            log.push("key");
            return "x";
        },
    };
    const source = {
        get x() {
            log.push("getter");
            return undefined;
        },
    };
    const run = new Function(
        "scope",
        "source",
        "sourceKey",
        "defaultValue",
        "var target; with (scope) { var { [sourceKey]: target = defaultValue } = source; } return target;"
    );
    expect(run(scope, source, sourceKey, 3)).toBe(3);
    expect(log).toEqual([
        "binding:source",
        "binding:sourceKey",
        "key",
        "binding:target",
        "getter",
        "binding:defaultValue",
    ]);
});

test("destructuring targets retain their bindings through source side effects", () => {
    for (const [pattern, isArray, isRest] of [
        ["var { x: target } = source", false, false],
        ["({ x: target } = source)", false, false],
        ["var [target] = source", true, false],
        ["[target] = source", true, false],
        ["var { ...target } = source", false, true],
        ["({ ...target } = source)", false, true],
        ["var [...target] = source", true, true],
        ["[...target] = source", true, true],
    ]) {
        const scope = { target: 100 };
        const source = isArray
            ? {
                  *[Symbol.iterator]() {
                      delete scope.target;
                      yield 3;
                  },
              }
            : {
                  get x() {
                      delete scope.target;
                      return 3;
                  },
              };
        const run = new Function("scope", "source", `var target = 0; with (scope) { ${pattern}; } return target;`);
        expect(run(scope, source)).toBe(0);
        expect(scope.target).toEqual(isRest ? (isArray ? [3] : { x: 3 }) : 3);
    }
});
