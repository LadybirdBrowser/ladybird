// Holds a rendering update's recording in flight beside the event loop.
//
// whileRecordingInFlight(mutate, during) runs `mutate` in an animation frame callback, so that the rendering update
// paints, and holds the recording that update lets fly. `during` then runs in a task while the recording is held, and
// gets the frame scheduler's state then. The recording is released once `during` is done. No rendering opportunity
// comes meanwhile but those `during` injects: the tasks after a rendering task wait for the frame in flight with it.
// `hold` says what internals.holdNextFrame() holds.
async function whileRecordingInFlight(mutate, during, hold = "") {
    if (document.readyState !== "complete")
        await new Promise(resolve => window.addEventListener("load", resolve, { once: true }));
    await twoFramesLanded();
    return new Promise((resolve, reject) => {
        requestAnimationFrame(() => {
            internals.holdNextFrame(hold);
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

// Resolves in the first task after two rendering updates once nothing of theirs is in flight. A frame that is still in
// flight spends a hold that a test arms next, which is then not the hold of the rendering update the test means. No
// rendering opportunity comes meanwhile: a running animation asks for one frame after another, and where each comes as
// the last one lands, no task may ever find none in flight.
async function twoFramesLanded() {
    await twoFrames();
    internals.setManualRenderingOpportunities(true);
    do await nextTask();
    while (internals.frameSchedulerState() !== "idle");
    internals.setManualRenderingOpportunities(false);
}

// Has the clock lane of the document's presented frame sample its animations beside a task.
//
// whileLaneAnimates(animate, during) injects rendering opportunities until the animation `animate` starts is running and
// a rendering update has left the document a plan for the lane of its frame, then runs `during` in the first task after
// the update's frame has landed and its lane, not that of an earlier frame, has come together from it, beside which the
// lane ticks. A tick a test injects before then reaches the earlier lane. `during` gets the frame time of the last
// rendering update, from which it injects the clock's ticks, and the animation's start time, read before the task.
// Rendering opportunities stay manual until `during` is done.
async function whileLaneAnimates(animate, during) {
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
        while (internals.frameSchedulerState() !== "idle" || internals.clockLaneIsComing(document));
        return await during(frameTime, animation, startTime);
    } finally {
        internals.setManualRenderingOpportunities(false);
    }
}

// Resolves once the srcdoc document of `iframe` has loaded, which it may have before the page's DOMContentLoaded, as the
// test begins.
function srcdocLoaded(iframe) {
    const document = iframe.contentDocument;
    if (document && document.URL === "about:srcdoc" && document.readyState === "complete") return Promise.resolve();
    return new Promise(resolve => iframe.addEventListener("load", resolve, { once: true }));
}

// Has the clock lane of the document's presented frame follow the pointer beside a task.
//
// whileLaneHovers(during) injects rendering opportunities until a rendering update has left the document a plan for the
// lane of its frame, then runs `during` in the first task after the update's frame has landed and its lane, not that of
// an earlier frame, has come together from it, beside which the lane hovers. A pointer move a test injects before the
// lane follows the pointer reaches no lane, and nothing else hands it to the host. Rendering opportunities stay manual
// until `during` is done. `documents` are those whose lanes `during` needs: a same-process iframe's document is laid
// out after the document around it, and its frame lands on its own. `frameTime`, if given, replaces the current time as
// the frame time of the rendering update.
async function whileLaneHovers(during, documents = [document], frameTime = undefined) {
    if (document.readyState !== "complete")
        await new Promise(resolve => window.addEventListener("load", resolve, { once: true }));
    internals.setManualRenderingOpportunities(true);
    try {
        await nextTask();
        frameTime ??= performance.now();
        internals.injectRenderingOpportunity(frameTime);
        do await nextTask();
        while (
            internals.frameSchedulerState() !== "idle" ||
            documents.some(doc => internals.clockLaneState(doc) === "none" || internals.clockLaneIsComing(doc))
        );
        return await during(frameTime);
    } finally {
        internals.setManualRenderingOpportunities(false);
    }
}

// Has a lane hover (x, y) beside a task, over the frame of a rendering update it injects, and answers who presents the
// document's last frame then: the clock, or the commit, where the seal found no hover could restyle anything in the frame.
async function hoverSeal(x, y) {
    // NB: The lane an earlier move left would do for whileLaneHovers(), before the frame of what the test just wrote.
    await twoFramesLanded();
    return whileLaneHovers(frameTime => {
        internals.injectHoverPointer(x, y, frameTime + 16);
        return internals.lastFramePresentedBy(document);
    });
}
