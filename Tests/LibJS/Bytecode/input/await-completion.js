async function awaitValue(value) {
    return await value;
}

async function disposeResource(value) {
    await using resource = value;
}

awaitValue(1);
disposeResource(null);
