export function createMediaElementSource(log, ui) {
    const {
        h,
        chooseMediaRow,
        createInfoRow,
        formatBytes,
        formatBitrate,
        setupMediaElementFilePicker,
        safeDisconnect,
    } = ui;

    let ctx = null;
    const media = document.createElement("audio");
    media.preload = "auto";
    media.loop = true;
    media.controls = false;
    media.style.display = "none";

    let source = null;
    let filePicker = null;
    let hasSelectedFile = false;
    let selectedFile = null;
    let selectedUrl = "../../Assets/tinkle.mp3";
    let decodeNonce = 0;

    const uiRoot = h("div", null);

    const mediaPanel = h("div", "panel");
    mediaPanel.style.boxShadow = "none";
    mediaPanel.style.background = "rgba(0, 0, 0, 0.12)";

    const mediaBd = h("div", "bd");

    const fileInput = document.createElement("input");
    fileInput.type = "file";
    fileInput.accept = "audio/*";
    fileInput.style.display = "none";

    const defaultPreset = { text: "tinkle.mp3", href: "../../Assets/tinkle.mp3" };

    function openFilePicker() {
        if (typeof fileInput.showPicker === "function") {
            fileInput.showPicker();
        } else {
            fileInput.click();
        }
    }

    const mediaRow = chooseMediaRow({
        labelText: defaultPreset.text,
        buttons: [
            {
                text: "Choose...",
                onClick: () => openFilePicker(),
            },
        ],
    });

    mediaRow.row.append(fileInput);
    mediaBd.append(media);
    mediaBd.append(mediaRow.row);
    mediaPanel.append(mediaBd);

    uiRoot.append(mediaPanel);

    const detailsFileSize = createInfoRow("File size");
    //const detailsSampleRate = createInfoRow("Sample rate");
    const detailsChannels = createInfoRow("Channels");
    const detailsLength = createInfoRow("Length");
    const detailsBitrate = createInfoRow("Bit rate");

    uiRoot.append(detailsFileSize.row);
    //uiRoot.append(detailsSampleRate.row);
    uiRoot.append(detailsChannels.row);
    uiRoot.append(detailsLength.row);
    uiRoot.append(detailsBitrate.row);

    function resetDetails() {
        detailsFileSize.value.textContent = "-";
        //detailsSampleRate.value.textContent = "-";
        detailsChannels.value.textContent = "-";
        detailsLength.value.textContent = "-";
        detailsBitrate.value.textContent = "-";
    }

    async function decodeSelectedFileIfPossible() {
        if (!ctx) return;

        if (!selectedFile && !selectedUrl) return;

        const myNonce = ++decodeNonce;
        try {
            let ab = null;
            let size = null;

            if (selectedFile) {
                ab = await selectedFile.arrayBuffer();
                size = selectedFile.size;
            } else if (selectedUrl) {
                const response = await fetch(selectedUrl);
                const blob = await response.blob();
                ab = await blob.arrayBuffer();
                size = blob.size;
            }

            if (!ab) return;

            const decoded = await ctx.decodeAudioData(ab);

            // Ignore stale decode results.
            if (myNonce !== decodeNonce) return;

            if (Number.isFinite(size)) detailsFileSize.value.textContent = formatBytes(size);
            //detailsSampleRate.value.textContent = `${decoded.sampleRate} Hz`;
            detailsChannels.value.textContent = String(decoded.numberOfChannels);
            detailsLength.value.textContent = `${decoded.length} frames (${decoded.duration.toFixed(3)} s)`;

            const dur = decoded.duration;
            if (Number.isFinite(dur) && dur > 0 && Number.isFinite(size)) {
                const avgBps = (size * 8) / dur;
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

    function setPresetSource(href) {
        selectedUrl = href;
        selectedFile = null;
        hasSelectedFile = true;

        if (filePicker) filePicker.teardown();

        const nameMatch = href ? href.split("/").pop() : "";
        mediaRow.label.textContent = nameMatch || "-";

        try {
            media.src = href;
            media.load();
        } catch (e) {
            log(`media.src set failed: ${e}`);
        }

        detailsFileSize.value.textContent = "decoding...";
        //detailsSampleRate.value.textContent = "decoding...";
        detailsChannels.value.textContent = "decoding...";
        detailsLength.value.textContent = "decoding...";
        detailsBitrate.value.textContent = "decoding...";
        decodeSelectedFileIfPossible();
    }

    setPresetSource(defaultPreset.href);

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
                    if (!hasFile) {
                        hasSelectedFile = !!selectedFile || !!selectedUrl;
                        return;
                    }
                    hasSelectedFile = true;
                },
                onFileSelected: file => {
                    selectedUrl = null;
                    selectedFile = file;
                    hasSelectedFile = true;
                    mediaRow.label.textContent = file.name || "(unnamed)";
                    detailsFileSize.value.textContent = formatBytes(file.size);
                    //detailsSampleRate.value.textContent = "decoding...";
                    detailsChannels.value.textContent = "decoding...";
                    detailsLength.value.textContent = "decoding...";
                    detailsBitrate.value.textContent = "decoding...";
                    decodeSelectedFileIfPossible();
                },
            });
        }

        // If a source was picked before this node was created, decode now.
        decodeSelectedFileIfPossible();

        if (ctx && ctx.state === "running") {
            try {
                const p = media.play();
                if (p && typeof p.then === "function") {
                    p.then(() => log("media.play ok")).catch(e => log(`media.play failed: ${e}`));
                }
            } catch (e) {
                log(`media.play failed: ${e}`);
            }
        }
    }

    return {
        kind: "mediaElement",
        title: "MediaElementAudioSourceNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        serializeState() {
            return { loop: media.loop };
        },
        applyState(state) {
            if (!state) return;
            if (state.loop !== undefined) media.loop = !!state.loop;
        },
        disconnectAll() {
            if (source) safeDisconnect(source);
        },
        outputNode() {
            return source;
        },
        onContextSuspended() {
            try {
                media.pause();
            } catch (e) {
                log(`media.pause failed: ${e}`);
            }
        },
        onContextResumed() {
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

            safeDisconnect(source);
            source = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
