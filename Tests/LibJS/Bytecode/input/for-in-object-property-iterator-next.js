function collect(object) {
    const keys = [];
    for (const key in object)
        keys.push(key);
    return keys;
}

collect({ 2: "two", foo: "foo", 7: "seven" });
