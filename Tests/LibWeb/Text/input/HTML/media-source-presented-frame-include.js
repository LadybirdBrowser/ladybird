// The red and blue streams are 12 seconds of 64x36 VP9 at 10fps, with every frame a keyframe so that append windows
// cut exactly. The left half is the stream's color, and the right half is gray at 60 times the whole second mod 5.
const COLORED_STREAM_TYPE = 'video/webm; codecs="vp9"';

async function fetchColoredStream(color) {
    const response = await fetch(`../../../Assets/${color}-with-second-in-gray-64x36.webm`);
    if (!response.ok) throw new Error(`fetching the ${color} stream failed with HTTP ${response.status}`);
    return new Uint8Array(await response.arrayBuffer());
}

async function openColoredStreamSource(video) {
    const mediaSource = new MediaSource();
    video.src = URL.createObjectURL(mediaSource);
    await new Promise(resolve => mediaSource.addEventListener("sourceopen", resolve, { once: true }));
    const sourceBuffer = mediaSource.addSourceBuffer(COLORED_STREAM_TYPE);
    return { mediaSource, sourceBuffer };
}

function waitForUpdateEnd(sourceBuffer) {
    return new Promise((resolve, reject) => {
        sourceBuffer.addEventListener("updateend", resolve, { once: true });
        sourceBuffer.addEventListener("error", () => reject(new Error("error event on SourceBuffer")), { once: true });
    });
}

async function appendWithinWindow(sourceBuffer, data, windowStart = 0, windowEnd = Infinity) {
    sourceBuffer.appendWindowEnd = Infinity;
    sourceBuffer.appendWindowStart = windowStart;
    sourceBuffer.appendWindowEnd = windowEnd;
    sourceBuffer.appendBuffer(data);
    await waitForUpdateEnd(sourceBuffer);
}

async function seekAndWait(video, time) {
    const seeked = new Promise(resolve => video.addEventListener("seeked", resolve, { once: true }));
    video.currentTime = time;
    await seeked;
}

function samplePresentedFrame(video) {
    const canvas = document.createElement("canvas");
    canvas.width = 64;
    canvas.height = 36;
    const context = canvas.getContext("2d", { willReadFrequently: true });
    context.drawImage(video, 0, 0);
    const [red, , blue] = context.getImageData(16, 18, 1, 1).data;
    let color = "unknown";
    if (red > 150 && blue < 100) color = "red";
    else if (blue > 150 && red < 100) color = "blue";
    const gray = context.getImageData(48, 18, 1, 1).data[1];
    return `${color} ${Math.round(gray / 60)}`;
}

// Waits a bounded time for the presented frame to match, so that a stale frame prints instead of timing out.
async function samplePresentedFrameOnceItMatches(video, expected) {
    await waitForCondition(() => samplePresentedFrame(video) === expected, 40, 25);
    return samplePresentedFrame(video);
}
