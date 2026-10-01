// Generates a WebM stream with a single Opus track, so that the byte size of every coded frame is known.

// NB: This must match the media server's byte capacity for a SourceBuffer with one audio track.
const AUDIO_SOURCE_BUFFER_CAPACITY = 12 * 1024 * 1024;
const GENERATED_FRAME_DURATION_MS = 20;

function ebmlSizeBytes(size) {
    const bytes = [0x01];
    for (let shift = 48; shift >= 0; shift -= 8) bytes.push(Math.floor(size / 2 ** shift) & 0xff);
    return bytes;
}

function ebmlElement(id, payload) {
    return [...id, ...ebmlSizeBytes(payload.length), ...payload];
}

function ebmlUintElement(id, value) {
    return ebmlElement(id, [(value >> 24) & 0xff, (value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff]);
}

function ebmlStringElement(id, string) {
    return ebmlElement(
        id,
        Array.from(string, character => character.charCodeAt(0))
    );
}

function ebmlFloatElement(id, value) {
    const bytes = new Uint8Array(8);
    new DataView(bytes.buffer).setFloat64(0, value);
    return ebmlElement(id, Array.from(bytes));
}

function generatedOpusInitializationSegment() {
    // OpusHead with one channel, no pre-skip, and a 48kHz input sample rate.
    const opusHead = [0x4f, 0x70, 0x75, 0x73, 0x48, 0x65, 0x61, 0x64, 1, 1, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0];
    const ebmlHeader = ebmlElement(
        [0x1a, 0x45, 0xdf, 0xa3],
        [
            ...ebmlUintElement([0x42, 0x86], 1),
            ...ebmlUintElement([0x42, 0xf7], 1),
            ...ebmlUintElement([0x42, 0xf2], 4),
            ...ebmlUintElement([0x42, 0xf3], 8),
            ...ebmlStringElement([0x42, 0x82], "webm"),
            ...ebmlUintElement([0x42, 0x87], 4),
            ...ebmlUintElement([0x42, 0x85], 2),
        ]
    );
    const unknownSizeSegment = [0x18, 0x53, 0x80, 0x67, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff];
    const info = ebmlElement([0x15, 0x49, 0xa9, 0x66], ebmlUintElement([0x2a, 0xd7, 0xb1], 1000000));
    const tracks = ebmlElement(
        [0x16, 0x54, 0xae, 0x6b],
        ebmlElement(
            [0xae],
            [
                ...ebmlUintElement([0xd7], 1),
                ...ebmlUintElement([0x73, 0xc5], 1),
                ...ebmlUintElement([0x83], 2),
                ...ebmlStringElement([0x86], "A_OPUS"),
                ...ebmlElement([0x63, 0xa2], opusHead),
                ...ebmlElement([0xe1], [...ebmlFloatElement([0xb5], 48000), ...ebmlUintElement([0x9f], 1)]),
            ]
        )
    );
    return new Uint8Array([...ebmlHeader, ...unknownSizeSegment, ...info, ...tracks]);
}

// Each frame is a CELT-only 20ms Opus packet, padded with zeroes to its size.
function generatedOpusCluster(firstFrame, frameSizes) {
    const clusterTimeMs = firstFrame * GENERATED_FRAME_DURATION_MS;
    const payload = ebmlUintElement([0xe7], clusterTimeMs);
    frameSizes.forEach((frameSize, index) => {
        const relativeTimeMs = index * GENERATED_FRAME_DURATION_MS;
        const block = new Array(4 + frameSize).fill(0);
        block[0] = 0x81;
        block[1] = (relativeTimeMs >> 8) & 0xff;
        block[2] = relativeTimeMs & 0xff;
        block[3] = 0x80;
        block[4] = 0xfc;
        payload.push(...ebmlElement([0xa3], block));
    });
    return new Uint8Array(ebmlElement([0x1f, 0x43, 0xb6, 0x75], payload));
}

function appendAndWait(sourceBuffer, data) {
    sourceBuffer.appendBuffer(data);
    return new Promise((resolve, reject) => {
        sourceBuffer.addEventListener("updateend", resolve, { once: true });
        sourceBuffer.addEventListener("error", () => reject(new Error("error event on SourceBuffer")), { once: true });
    });
}
