export function createMediaStreamSource(log, ui) {
    const { h, chooseMediaRow, createInfoRow, createSelectRow, safeDisconnect } = ui;

    let ctx = null;
    let stream = null;
    let source = null;
    let selectedDeviceId = "";
    let captureWanted = false;
    let onGraphChanged = null;

    const uiRoot = h("div", null);

    const deviceRow = createSelectRow({
        label: "Microphone",
        optionsList: [{ value: "", text: "(default)", selected: true }],
        onChange: value => {
            selectedDeviceId = value || "";
            if (captureWanted) startCapture();
        },
    });

    const controlRow = chooseMediaRow({
        labelText: "Capture",
        buttons: [
            { text: "Start mic", className: "primary" },
            { text: "Refresh" },
        ],
    });

    const btnToggle = controlRow.buttons[0];
    const btnRefresh = controlRow.buttons[1];

    uiRoot.append(deviceRow.row);
    uiRoot.append(controlRow.row);

    const detailsDevice = createInfoRow("Selected device");
    const detailsTrackState = createInfoRow("Track state");
    const detailsChannels = createInfoRow("Channels");
    const detailsSampleRate = createInfoRow("Sample rate");

    uiRoot.append(detailsDevice.row);
    uiRoot.append(detailsTrackState.row);
    uiRoot.append(detailsChannels.row);
    uiRoot.append(detailsSampleRate.row);

    function updateButtons() {
        const active = !!stream;
        btnToggle.textContent = active ? "Stop mic" : "Start mic";
        btnToggle.classList.toggle("danger", active);
        btnToggle.classList.toggle("primary", !active);
    }

    function resetDetails() {
        detailsDevice.value.textContent = "-";
        detailsTrackState.value.textContent = "-";
        detailsChannels.value.textContent = "-";
        detailsSampleRate.value.textContent = "-";
    }

    function updateDetailsFromStream() {
        if (!stream) {
            resetDetails();
            return;
        }
        const track = stream.getAudioTracks()[0];
        if (!track) {
            resetDetails();
            return;
        }

        const settings = track.getSettings ? track.getSettings() : {};
        const label = track.label || "(unlabeled device)";
        const channels = settings.channelCount ? String(settings.channelCount) : "-";
        const sampleRate = settings.sampleRate ? `${settings.sampleRate} Hz` : "-";

        detailsDevice.value.textContent = label;
        detailsTrackState.value.textContent = track.readyState || "-";
        detailsChannels.value.textContent = channels;
        detailsSampleRate.value.textContent = sampleRate;
    }

    function notifyGraphChanged() {
        if (onGraphChanged) onGraphChanged();
    }

    function stopCapture() {
        captureWanted = false;

        if (stream) {
            for (const track of stream.getTracks()) {
                try {
                    track.stop();
                } catch (_) {}
            }
        }

        stream = null;
        safeDisconnect(source);
        source = null;

        updateButtons();
        updateDetailsFromStream();
        notifyGraphChanged();
    }

    async function startCapture() {
        captureWanted = true;

        if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
            log("getUserMedia is unavailable");
            stopCapture();
            return;
        }

        if (!ctx) return;

        if (stream) stopCapture();

        const audioConstraints = selectedDeviceId ? { deviceId: { exact: selectedDeviceId } } : true;

        try {
            stream = await navigator.mediaDevices.getUserMedia({ audio: audioConstraints, video: false });
        } catch (e) {
            log(`getUserMedia failed: ${e}`);
            stopCapture();
            return;
        }

        try {
            source = ctx.createMediaStreamSource(stream);
        } catch (e) {
            log(`createMediaStreamSource failed: ${e}`);
            stopCapture();
            return;
        }

        updateButtons();
        updateDetailsFromStream();
        notifyGraphChanged();
    }

    async function refreshDevices() {
        if (!navigator.mediaDevices || !navigator.mediaDevices.enumerateDevices) {
            log("enumerateDevices is unavailable");
            return;
        }

        try {
            const devices = await navigator.mediaDevices.enumerateDevices();
            const audioInputs = devices.filter(d => d.kind === "audioinput");

            deviceRow.select.innerHTML = "";
            const defaultOpt = document.createElement("option");
            defaultOpt.value = "";
            defaultOpt.textContent = "(default)";
            deviceRow.select.append(defaultOpt);

            let hasSelected = false;
            audioInputs.forEach((device, index) => {
                const opt = document.createElement("option");
                opt.value = device.deviceId;
                const label = device.label || `Microphone ${index + 1}`;
                opt.textContent = label;
                if (device.deviceId && device.deviceId === selectedDeviceId) {
                    opt.selected = true;
                    hasSelected = true;
                }
                deviceRow.select.append(opt);
            });

            if (!hasSelected) {
                selectedDeviceId = "";
                deviceRow.select.value = "";
            }
        } catch (e) {
            log(`enumerateDevices failed: ${e}`);
        }
    }

    btnToggle.addEventListener("click", () => {
        if (stream) {
            stopCapture();
        } else {
            startCapture();
        }
    });

    btnRefresh.addEventListener("click", () => refreshDevices());

    resetDetails();
    updateButtons();

    return {
        kind: "mediaStream",
        title: "MediaStreamAudioSourceNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
            refreshDevices();
        },
        setOnGraphChanged(cb) {
            onGraphChanged = cb;
        },
        ensureAudio() {
            if (!ctx) return;
            refreshDevices();
        },
        serializeState() {
            return { deviceId: selectedDeviceId };
        },
        applyState(state) {
            if (!state) return;
            if (state.deviceId !== undefined) selectedDeviceId = String(state.deviceId || "");
        },
        disconnectAll() {
            if (source) safeDisconnect(source);
        },
        outputNode() {
            return source;
        },
        onContextSuspended() {
            if (!captureWanted) return;
            stopCapture();
            captureWanted = true;
        },
        onContextResumed() {
            if (captureWanted && !stream) startCapture();
        },
        teardownAudio() {
            stopCapture();
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
