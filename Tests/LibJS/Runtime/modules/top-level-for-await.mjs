const log = [];

for await (const value of [1, Promise.resolve(2)]) {
    log.push(value);
}

async function* letters() {
    try {
        yield "a";
        yield "b";
        yield "c";
    } finally {
        log.push("closed");
    }
}

for await (const letter of letters()) {
    log.push(letter);
    if (letter === "b") break;
}

async function* failing() {
    yield "first";
    throw new Error("iterator failed");
}

try {
    for await (const value of failing()) {
        log.push(value);
    }
} catch (error) {
    log.push(error.message);
}

outer: for await (const number of [1, 2]) {
    for await (const letter of ["x", "y"]) {
        if (letter === "y") continue outer;
        log.push(`${number}${letter}`);
    }
}

export const result = log;
export const passed = true;
