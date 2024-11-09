test("basic functionality", () => {
    const genericStringPrototypeFunctions = [
        "charAt",
        "charCodeAt",
        "repeat",
        "startsWith",
        "endsWith",
        "indexOf",
        "toLowerCase",
        "toUpperCase",
        "padStart",
        "padEnd",
        "trim",
        "trimStart",
        "trimEnd",
        "concat",
        "substring",
        "includes",
        "slice",
    ];

    genericStringPrototypeFunctions.forEach(name => {
        String.prototype[name].call({ toString: () => "hello friends" });
        String.prototype[name].call({ toString: () => 123 });
        String.prototype[name].call({ toString: () => undefined });

        expect(() => {
            String.prototype[name].call({ toString: () => new String() });
        }).toThrowWithMessage(TypeError, "Cannot convert object to string");

        expect(() => {
            String.prototype[name].call({ toString: () => [] });
        }).toThrowWithMessage(TypeError, "Cannot convert object to string");

        expect(() => {
            String.prototype[name].call({ toString: () => ({}) });
        }).toThrowWithMessage(TypeError, "Cannot convert object to string");
    });
});
