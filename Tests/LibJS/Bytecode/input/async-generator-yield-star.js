async function* delegate(value) {
    return yield* value;
}

delegate([1]).next();
