export function createAnalyserProcessor(log, ui) {
    const {
        h,
        createSelectRow,
        createRangeNumberRow,
        clampNumber,
        createTimeDomainOscilloscope,
        createFrequencyDomainSpectrum,
        safeDisconnect,
    } = ui;

    let ctx = null;
    let node = null;
    let timeRenderer = null;
    let freqRenderer = null;
    let visualizersEnabled = false;

    const uiRoot = h("div", null);

    const fftRow = createSelectRow({
        label: "FFT size",
        optionsList: [
            { value: "256", text: "256" },
            { value: "512", text: "512" },
            { value: "1024", text: "1024" },
            { value: "2048", text: "2048", selected: true },
            { value: "4096", text: "4096" },
            { value: "8192", text: "8192" },
        ],
        onChange: () => apply(),
    });

    const smoothRow = createRangeNumberRow({
        label: "Smoothing",
        range: { min: "0", max: "100", value: "80", step: "1" },
        number: { min: "0", max: "1", value: "0.8", step: "0.01" },
        onChange: () => apply(),
    });
    smoothRow.rangeEl.addEventListener("input", () => {
        const pct = Number(smoothRow.rangeEl.value);
        if (Number.isFinite(pct)) smoothRow.numberEl.value = (pct / 100).toFixed(2);
        smoothRow.numberEl.dispatchEvent(new Event("input"));
    });
    smoothRow.numberEl.addEventListener("input", () => {
        const v = Number(smoothRow.numberEl.value);
        const s = Number.isFinite(v) ? clampNumber(v, 0, 1) : 0.8;
        smoothRow.rangeEl.value = String(Math.round(s * 100));
    });

    const viz = h("div", "viz");

    const timeCanvas = document.createElement("canvas");
    timeCanvas.width = 1000;
    timeCanvas.height = 280;

    const freqCanvas = document.createElement("canvas");
    freqCanvas.width = 1000;
    freqCanvas.height = 280;

    viz.append(timeCanvas);
    viz.append(freqCanvas);

    uiRoot.append(fftRow.row);
    uiRoot.append(smoothRow.row);
    uiRoot.append(viz);

    function ensureAudio(context) {
        if (node) return;
        ctx = context;
        node = ctx.createAnalyser();

        timeRenderer = createTimeDomainOscilloscope({
            canvas: timeCanvas,
            log,
            setPill: () => {},
        });
        freqRenderer = createFrequencyDomainSpectrum({
            canvas: freqCanvas,
            log,
            setPill: () => {},
        });

        apply();
        updateViz();
    }

    function apply() {
        if (!node) return;

        const fft = Number(fftRow.select.value);
        const smoothing = Number(smoothRow.numberEl.value);

        try {
            if (Number.isFinite(fft)) node.fftSize = fft;
        } catch (e) {
            log(`analyser.fftSize set failed: ${e}`);
        }
        try {
            if (Number.isFinite(smoothing)) node.smoothingTimeConstant = clampNumber(smoothing, 0, 1);
        } catch (e) {
            log(`analyser.smoothingTimeConstant set failed: ${e}`);
        }

        updateViz();
    }

    function updateViz() {
        if (!timeRenderer || !freqRenderer) return;

        if (!visualizersEnabled || !node) {
            timeRenderer.stop();
            freqRenderer.stop();
            return;
        }

        timeRenderer.start({ analyser: node, enabled: true });
        freqRenderer.start({ analyser: node, enabled: true });
    }

    return {
        kind: "analyser",
        title: "AnalyserNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        serializeState() {
            return {
                fftSize: fftRow.select.value,
                smoothing: smoothRow.numberEl.value,
            };
        },
        applyState(state) {
            if (!state) return;
            if (state.fftSize !== undefined) fftRow.select.value = String(state.fftSize);
            if (state.smoothing !== undefined) smoothRow.numberEl.value = String(state.smoothing);
            fftRow.select.dispatchEvent(new Event("change"));
            smoothRow.numberEl.dispatchEvent(new Event("input"));
        },
        setVisualizersEnabled(enabled) {
            visualizersEnabled = enabled;
            updateViz();
        },
        disconnectAll() {
            safeDisconnect(node);
        },
        audioNode() {
            if (!node) throw new Error("AnalyserNode missing");
            return node;
        },
        outputNode() {
            if (!node) throw new Error("AnalyserNode missing");
            return node;
        },
        teardownAudio() {
            if (timeRenderer) timeRenderer.stop();
            if (freqRenderer) freqRenderer.stop();
            timeRenderer = null;
            freqRenderer = null;
            safeDisconnect(node);
            node = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
