function forOf(rows) {
    for (const row of rows) {
        for (const cell of row) {
            console.log(cell);
        }
    }
}

function forIn(object) {
    for (const key in object) {
        for (const inner in object[key]) {
            console.log(key, inner);
        }
    }
}

async function forAwait(rows) {
    for await (const row of rows) {
        for await (const cell of row) {
            console.log(cell);
        }
    }
}

forOf([]);
forIn({});
forAwait([]);
