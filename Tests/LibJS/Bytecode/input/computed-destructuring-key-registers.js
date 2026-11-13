function without_rest(source, key) {
    const { [key]: first, [key]: second, [key]: third, [key]: fourth } = source;
    return [first, second, third, fourth];
}

function with_rest(source, key) {
    const { [key]: first, [key]: second, ...rest } = source;
    return [first, second, rest];
}

without_rest({ value: 1 }, "value");
with_rest({ value: 1, other: 2 }, "value");
