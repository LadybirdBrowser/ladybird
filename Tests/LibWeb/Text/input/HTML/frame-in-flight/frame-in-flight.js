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

// Leases the document's render state to the render clock for a task.
//
// whileClockLeased(animate, during) injects rendering opportunities until the animation `animate` starts is running and
// a rendering update has left the document a plan for a clock lease, then runs `during` in the first task after the
// update's frame has landed, which the lease begins with. `during` gets the frame time of the last rendering update,
// from which it injects the clock's ticks, and the animation's start time, read before the lease: a read of a CSS
// animation's timing reads its style, which ends the lease. Rendering opportunities stay manual until `during` is done.
async function whileClockLeased(animate, during) {
    if (document.readyState !== "complete")
        await new Promise(resolve => window.addEventListener("load", resolve, { once: true }));
    internals.setManualRenderingOpportunities(true);
    try {
        await nextTask();
        const animation = animate();
        let frameTime = performance.now();
        internals.injectRenderingOpportunity(frameTime);
        await nextTask();
        while (animation.pending) {
            frameTime += 16;
            internals.injectRenderingOpportunity(frameTime);
            await nextTask();
        }
        const startTime = animation.startTime;
        frameTime += 16;
        internals.injectRenderingOpportunity(frameTime);
        do await nextTask();
        while (internals.frameSchedulerState() !== "idle");
        return await during(frameTime, animation, startTime);
    } finally {
        internals.setManualRenderingOpportunities(false);
    }
}

// Leases the document's render state to the render clock for a task, for its ticks to follow the pointer.
//
// whileHoverLeased(during) injects rendering opportunities until a rendering update has left the document a plan for a
// clock lease, then runs `during` in the first task after the update's frame has landed, which the lease begins with.
// Rendering opportunities stay manual until `during` is done.
async function whileHoverLeased(during) {
    if (document.readyState !== "complete")
        await new Promise(resolve => window.addEventListener("load", resolve, { once: true }));
    internals.setManualRenderingOpportunities(true);
    try {
        await nextTask();
        let frameTime = performance.now();
        internals.injectRenderingOpportunity(frameTime);
        do await nextTask();
        while (internals.frameSchedulerState() !== "idle");
        return await during(frameTime);
    } finally {
        internals.setManualRenderingOpportunities(false);
    }
}
