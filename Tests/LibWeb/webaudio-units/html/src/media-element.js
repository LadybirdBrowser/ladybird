export function createMediaElementSource(log, ui) {
    const { h, createInfoRow, formatBytes, formatBitrate, setupMediaElementFilePicker, safeDisconnect } = ui;

    /** @type {AudioContext|null} */
    let ctx = null;
    /** @type {HTMLAudioElement} */
    const media = /** @type {HTMLAudioElement} */ (document.createElement("audio"));
    media.preload = "auto";
    media.loop = true;
    media.controls = true;

    /** @type {MediaElementAudioSourceNode|null} */
    let source = null;

    /** @type {ReturnType<typeof setupMediaElementFilePicker>|null} */
    let filePicker = null;
    let hasSelectedFile = false;
    let resumeWanted = false;

    /** @type {File|null} */
    let selectedFile = null;

    /** @type {number} */
    let decodeNonce = 0;

    const uiRoot = h("div", null);

    const mediaPanel = h("div", "panel");
    mediaPanel.style.boxShadow = "none";
    mediaPanel.style.background = "rgba(0, 0, 0, 0.12)";

    const mediaBd = h("div", "bd");
    const row = h("div", "flex-row");

    const fileInput = /** @type {HTMLInputElement} */ (document.createElement("input"));
    fileInput.type = "file";
    fileInput.accept = "audio/*";

    row.append(fileInput);
    mediaBd.append(media);
    mediaBd.append(row);
    mediaPanel.append(mediaBd);

    uiRoot.append(mediaPanel);

    const detailsFileSize = createInfoRow("File size");
    const detailsSampleRate = createInfoRow("Sample rate");
    const detailsChannels = createInfoRow("Channels");
    const detailsLength = createInfoRow("Length");
    const detailsBitrate = createInfoRow("Bit rate");

    uiRoot.append(detailsFileSize.row);
    uiRoot.append(detailsSampleRate.row);
    uiRoot.append(detailsChannels.row);
    uiRoot.append(detailsLength.row);
    uiRoot.append(detailsBitrate.row);

    function resetDetails() {
        detailsFileSize.value.textContent = "-";
        detailsSampleRate.value.textContent = "-";
        detailsChannels.value.textContent = "-";
        detailsLength.value.textContent = "-";
        detailsBitrate.value.textContent = "-";
    }

    async function decodeSelectedFileIfPossible() {
        if (!ctx || !selectedFile) return;

        const myNonce = ++decodeNonce;
        try {
            const ab = await selectedFile.arrayBuffer();
            const decoded = await ctx.decodeAudioData(ab);

            // Ignore stale decode results.
            if (myNonce !== decodeNonce) return;

            detailsSampleRate.value.textContent = `${decoded.sampleRate} Hz`;
            detailsChannels.value.textContent = String(decoded.numberOfChannels);
            detailsLength.value.textContent = `${decoded.length} frames (${decoded.duration.toFixed(3)} s)`;

            const dur = decoded.duration;
            if (Number.isFinite(dur) && dur > 0 && selectedFile) {
                const avgBps = (selectedFile.size * 8) / dur;
                detailsBitrate.value.textContent = `${formatBitrate(avgBps)} (avg)`;
            } else {
                detailsBitrate.value.textContent = "-";
            }
        } catch (e) {
            if (myNonce !== decodeNonce) return;
            resetDetails();
            if (selectedFile) detailsFileSize.value.textContent = formatBytes(selectedFile.size);
            log(`decodeAudioData (media details) failed: ${e}`);
        }
    }

    function ensureAudio(context) {
        if (source) return;

        ctx = context;
        try {
            source = ctx.createMediaElementSource(media);
        } catch (e) {
            log(`createMediaElementSource failed: ${e}`);
            source = null;
            return;
        }

        if (!filePicker) {
            filePicker = setupMediaElementFilePicker({
                fileInput,
                mediaElement: media,
                log,
                onSelectionChanged: hasFile => {
                    hasSelectedFile = hasFile;
                    if (!hasFile) {
                        selectedFile = null;
                        decodeNonce++;
                        resetDetails();
                    }
                },
                onFileSelected: file => {
                    selectedFile = file;
                    detailsFileSize.value.textContent = formatBytes(file.size);
                    detailsSampleRate.value.textContent = "decoding...";
                    detailsChannels.value.textContent = "decoding...";
                    detailsLength.value.textContent = "decoding...";
                    detailsBitrate.value.textContent = "decoding...";
                    decodeSelectedFileIfPossible();
                },
            });
        }

        // If a file was picked before this node was created, decode now.
        decodeSelectedFileIfPossible();
    }

    return {
        kind: "mediaElement",
        title: "MediaElementAudioSourceNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        disconnectAll() {
            if (source) safeDisconnect(source);
        },
        outputNode() {
            return source;
        },
        onContextSuspended() {
            resumeWanted = !media.paused && !media.ended;
            try {
                media.pause();
            } catch (e) {
                log(`media.pause failed: ${e}`);
            }
        },
        onContextResumed() {
            if (!resumeWanted) return;
            resumeWanted = false;
            if (!hasSelectedFile) return;
            try {
                const p = media.play();
                if (p && typeof p.then === "function") {
                    p.then(() => log("media.play ok")).catch(e => log(`media.play failed: ${e}`));
                }
            } catch (e) {
                log(`media.play failed: ${e}`);
            }
        },
        teardownAudio() {
            try {
                media.pause();
            } catch (_) {}
            if (filePicker) filePicker.teardown();
            filePicker = null;
            hasSelectedFile = false;
            resumeWanted = false;

            safeDisconnect(source);
            source = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
