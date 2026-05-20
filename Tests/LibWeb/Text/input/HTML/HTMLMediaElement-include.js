const READY_STATE_NAMES = {
    [HTMLMediaElement.HAVE_NOTHING]: "HAVE_NOTHING",
    [HTMLMediaElement.HAVE_METADATA]: "HAVE_METADATA",
    [HTMLMediaElement.HAVE_CURRENT_DATA]: "HAVE_CURRENT_DATA",
    [HTMLMediaElement.HAVE_FUTURE_DATA]: "HAVE_FUTURE_DATA",
    [HTMLMediaElement.HAVE_ENOUGH_DATA]: "HAVE_ENOUGH_DATA",
};

const MIN_READY_STATE_FOR_EVENT = {
    loadstart: HTMLMediaElement.HAVE_NOTHING,
    abort: HTMLMediaElement.HAVE_NOTHING,
    error: HTMLMediaElement.HAVE_NOTHING,
    emptied: HTMLMediaElement.HAVE_NOTHING,
    durationchange: HTMLMediaElement.HAVE_METADATA,
    loadedmetadata: HTMLMediaElement.HAVE_METADATA,
    seeking: HTMLMediaElement.HAVE_METADATA,
    seeked: HTMLMediaElement.HAVE_METADATA,
    ended: HTMLMediaElement.HAVE_METADATA,
    loadeddata: HTMLMediaElement.HAVE_CURRENT_DATA,
    canplay: HTMLMediaElement.HAVE_FUTURE_DATA,
    canplaythrough: HTMLMediaElement.HAVE_ENOUGH_DATA,
};

function readyStateName(value) {
    return READY_STATE_NAMES[value] ?? `<unknown:${value}>`;
}

function formatReadyStateExpectation(element, eventName) {
    if (!(eventName in MIN_READY_STATE_FOR_EVENT))
        throw new Error(`No minimum readyState defined for event "${eventName}"`);
    const min = MIN_READY_STATE_FOR_EVENT[eventName];
    const minName = readyStateName(min);
    return element.readyState >= min
        ? `readyState >= ${minName}`
        : `readyState=${readyStateName(element.readyState)}, expected >= ${minName}`;
}
