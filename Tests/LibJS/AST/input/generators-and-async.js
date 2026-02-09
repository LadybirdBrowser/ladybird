// Generator function locals.
function* counter(start) {
    let i = start;
    while (true) {
        yield i;
        i = i + 1;
    }
}

// Async function locals.
async function fetch_data(url) {
    let result = await url;
    return result;
}

// Async generator.
async function* stream(items) {
    for (let item of items) {
        yield item;
    }
}

// Generator with eval (poisons the generator's scope).
function* gen_with_eval() {
    let x = 1;
    eval("");
    yield x;
}
