export function createBufferSource(log, ui) {
    const {
        h,
        createInfoRow,
        createCheckboxRow,
        createRangeNumberRow,
        formatBytes,
        formatBitrate,
        clampNumber,
        setupMediaElementFilePicker,
        safeDisconnect,
    } = ui;

    /** @type {AudioContext|null} */
    let ctx = null;

    /** @type {AudioBuffer|null} */
    let buffer = null;

    /** @type {AudioBufferSourceNode|null} */
    let playingNode = null;

    /** @type {ReturnType<typeof setupMediaElementFilePicker>|null} */
    let filePicker = null;

    // We reuse the file picker helper by driving an <audio> element purely for URL lifecycle.
    // (We do not use it for playback.)
    const dummyMedia = /** @type {HTMLAudioElement} */ (document.createElement("audio"));

    let hasSelectedFile = false;
    /** @type {File|null} */
    let selectedFile = null;

    /** @type {number} */
    let decodeNonce = 0;

    /** @type {(() => void)|null} */
    let onGraphChanged = null;

    const uiRoot = h("div", null);

    const detailsFileSize = createInfoRow("File size");
    const detailsSampleRate = createInfoRow("Sample rate");
    const detailsChannels = createInfoRow("Channels");
    const detailsLength = createInfoRow("Length");
    const detailsBitrate = createInfoRow("Bit rate");

    const row = h("div", "flex-row");
    const fileInput = /** @type {HTMLInputElement} */ (document.createElement("input"));
    fileInput.type = "file";
    fileInput.accept = "audio/*";

    row.append(fileInput);

    const loopRow = createCheckboxRow({
        label: "Loop",
        checked: true,
        onChange: () => {
            if (playingNode) {
                try {
                    playingNode.loop = loopRow.cb.checked;
                } catch (_) {}
            }
        },
        hint: "",
    });

    const rateRow = createRangeNumberRow({
        label: "Playback rate",
        range: { min: "0", max: "400", value: "100", step: "1" },
        number: { min: "0", max: "4", value: "1", step: "0.01" },
        onChange: () => {
            const v = Number(rateRow.numberEl.value);
            const r = Number.isFinite(v) ? clampNumber(v, 0, 4) : 1;
            if (playingNode) {
                try {
                    playingNode.playbackRate.value = r;
                } catch (e) {
                    log(`playbackRate set failed: ${e}`);
                }
            }
        },
    });
    // Keep percent slider mapping.
    rateRow.rangeEl.addEventListener("input", () => {
        const pct = Number(rateRow.rangeEl.value);
        if (Number.isFinite(pct)) rateRow.numberEl.value = (pct / 100).toFixed(2);
        rateRow.numberEl.dispatchEvent(new Event("input"));
    });
    rateRow.numberEl.addEventListener("input", () => {
        const v = Number(rateRow.numberEl.value);
        const r = Number.isFinite(v) ? clampNumber(v, 0, 4) : 1;
        rateRow.rangeEl.value = String(Math.round(r * 100));
    });

    const detuneRow = createRangeNumberRow({
        label: "Detune (cents)",
        range: { min: "-1200", max: "1200", value: "0", step: "1" },
        number: { min: "-12000", max: "12000", value: "0", step: "1" },
        onChange: () => {
            const v = Number(detuneRow.numberEl.value);
            const d = Number.isFinite(v) ? v : 0;
            if (playingNode) {
                try {
                    playingNode.detune.value = d;
                } catch (e) {
                    log(`detune set failed: ${e}`);
                }
            }
        },
    });

    uiRoot.append(row);
    uiRoot.append(detailsFileSize.row);
    uiRoot.append(detailsSampleRate.row);
    uiRoot.append(detailsChannels.row);
    uiRoot.append(detailsLength.row);
    uiRoot.append(detailsBitrate.row);
    uiRoot.append(loopRow.row);
    uiRoot.append(rateRow.row);
    uiRoot.append(detuneRow.row);

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
            buffer = await ctx.decodeAudioData(ab);

            if (myNonce !== decodeNonce) return;

            log(
                `Decoded AudioBuffer: channels=${buffer.numberOfChannels}, length=${buffer.length}, sampleRate=${buffer.sampleRate}`
            );

            detailsFileSize.value.textContent = formatBytes(selectedFile.size);
            detailsSampleRate.value.textContent = `${buffer.sampleRate} Hz`;
            detailsChannels.value.textContent = String(buffer.numberOfChannels);
            detailsLength.value.textContent = `${buffer.length} frames (${buffer.duration.toFixed(3)} s)`;

            if (Number.isFinite(buffer.duration) && buffer.duration > 0) {
                const avgBps = (selectedFile.size * 8) / buffer.duration;
                detailsBitrate.value.textContent = `${formatBitrate(avgBps)} (avg)`;
            } else {
                detailsBitrate.value.textContent = "-";
            }

            // Playback is driven by global Start/Stop.
            maybeStartPlayback();
        } catch (e) {
            if (myNonce !== decodeNonce) return;
            buffer = null;
            log(`decodeAudioData failed: ${e}`);
            resetDetails();
            if (selectedFile) detailsFileSize.value.textContent = formatBytes(selectedFile.size);
        }
    }

    function ensureAudio(context) {
        ctx = context;

        if (!filePicker) {
            filePicker = setupMediaElementFilePicker({
                fileInput,
                mediaElement: dummyMedia,
                log,
                onSelectionChanged: hasFile => {
                    hasSelectedFile = hasFile;
                    if (!hasFile) {
                        selectedFile = null;
                        buffer = null;
                        decodeNonce++;
                        stopPlayingNode();
                        resetDetails();
                    }
                    buffer = null;
                },
                onFileSelected: file => {
                    hasSelectedFile = true;
                    selectedFile = file;
                    buffer = null;
                    stopPlayingNode();
                    detailsFileSize.value.textContent = formatBytes(file.size);
                    detailsSampleRate.value.textContent = "decoding...";
                    detailsChannels.value.textContent = "decoding...";
                    detailsLength.value.textContent = "decoding...";
                    detailsBitrate.value.textContent = "decoding...";
                    decodeSelectedFileIfPossible();
                },
            });
        }

        // If a file was picked before ctx existed, decode now.
        decodeSelectedFileIfPossible();

        // If we're already running and decoded, ensure playback started.
        maybeStartPlayback();
    }

    function stopPlayingNode() {
        if (!playingNode) return;
        try {
            playingNode.stop();
        } catch (_) {}
        safeDisconnect(playingNode);
        playingNode = null;
        if (onGraphChanged) onGraphChanged();
    }

    function maybeStartPlayback() {
        if (!ctx) return;
        if (ctx.state !== "running") return;
        if (!buffer) return;

        // If already playing, leave it alone.
        if (playingNode) return;

        play();
    }

    function play() {
        if (!ctx) return;
        if (ctx.state !== "running") {
            log("AudioContext is not running; click Start first.");
            return;
        }
        if (!buffer) {
            log("No decoded buffer yet.");
            return;
        }

        stopPlayingNode();

        const node = ctx.createBufferSource();
        node.buffer = buffer;
        node.loop = !!loopRow.cb.checked;

        try {
            node.playbackRate.value = clampNumber(Number(rateRow.numberEl.value), 0, 4);
        } catch (_) {}
        try {
            node.detune.value = Number(detuneRow.numberEl.value) || 0;
        } catch (_) {}

        node.onended = () => {
            playingNode = null;
            if (onGraphChanged) onGraphChanged();
        };

        try {
            node.start();
        } catch (e) {
            log(`bufferSource.start failed: ${e}`);
        }

        playingNode = node;
        if (onGraphChanged) onGraphChanged();
    }

    return {
        kind: "bufferSource",
        title: "AudioBufferSourceNode",
        uiRoot,
        setOnGraphChanged(fn) {
            onGraphChanged = fn;
        },
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        disconnectAll() {
            if (playingNode) safeDisconnect(playingNode);
        },
        outputNode() {
            return playingNode;
        },
        onContextSuspended() {
            stopPlayingNode();
        },
        onContextResumed() {
            maybeStartPlayback();
        },
        teardownAudio() {
            stopPlayingNode();
            if (filePicker) filePicker.teardown();
            filePicker = null;
            hasSelectedFile = false;
            selectedFile = null;
            buffer = null;
            resetDetails();
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
