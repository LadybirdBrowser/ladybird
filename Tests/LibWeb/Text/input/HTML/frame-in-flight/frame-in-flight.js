// Holds a rendering update's recording in flight beside the event loop.
//
// whileRecordingInFlight(mutate, during) runs `mutate` in an animation frame callback, so that the rendering update
// paints, and holds the recording that update lets fly. `during` then runs in a task while the recording is held, and
// gets the frame scheduler's state then. The recording is released once `during` is done. No rendering opportunity
// comes meanwhile but those `during` injects: the tasks after a rendering task wait for the frame in flight with it.
async function whileRecordingInFlight(mutate, during) {
    if (document.readyState !== "complete")
        await new Promise(resolve => window.addEventListener("load", resolve, { once: true }));
    await twoFrames();
    return new Promise((resolve, reject) => {
        requestAnimationFrame(() => {
            internals.holdNextFrame();
            internals.setManualRenderingOpportunities(true);
            mutate();
            setTimeout(async () => {
                try {
                    resolve(await during(internals.frameSchedulerState()));
                } catch (e) {
                    reject(e);
                } finally {
                    internals.releaseHeldFrame();
                    internals.setManualRenderingOpportunities(false);
                }
            }, 0);
        });
    });
}

function nextTask() {
    const { promise, resolve } = Promise.withResolvers();
    const channel = new MessageChannel();
    channel.port1.onmessage = resolve;
    channel.port2.postMessage(null);
    return promise;
}

function nextFrame() {
    return new Promise(resolve => requestAnimationFrame(() => resolve()));
}

async function twoFrames() {
    await nextFrame();
    await nextFrame();
}
