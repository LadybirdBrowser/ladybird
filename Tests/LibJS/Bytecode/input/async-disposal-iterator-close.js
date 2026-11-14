async function disposeThenClose(iterable, resource) {
    for await (const value of iterable) {
        await using held = resource;
        break;
    }
}

async function closeOnBreak(iterable) {
    for await (const value of iterable) {
        break;
    }
}

disposeThenClose([], null);
closeOnBreak([]);
