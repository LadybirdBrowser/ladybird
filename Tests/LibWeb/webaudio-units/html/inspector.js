import { createOscillatorSource } from "./src/oscillator.js";
import { createMediaElementSource } from "./src/media-element.js";
import { createBufferSource } from "./src/buffer-source.js";
import { createConstantSource } from "./src/constant-source.js";
import { createGainProcessor } from "./src/gain.js";
import { createDelayProcessor } from "./src/delay.js";
import { createStereoPannerProcessor } from "./src/stereo-panner.js";
import { createBiquadFilterProcessor } from "./src/biquad-filter.js";
import { createDynamicsCompressorProcessor } from "./src/dynamics-compressor.js";
import { createAnalyserProcessor } from "./src/analyser.js";
import { createOhNoesProcessor } from "./src/ohnoes.js";

(() => {
    const {
        createDefaultLogger,
        createPillController,
        clampNumber,
        parseNumber,
        syncRangeAndNumber,
        createTimeDomainOscilloscope,
        createFrequencyDomainSpectrum,
        setupMediaElementFilePicker,
        setupStatusTextLoop,
    } = window.WebAudio;

    const log = createDefaultLogger();

    const el = {
        graph: document.getElementById("graph"),
        btnToggle: /** @type {HTMLButtonElement} */ (document.getElementById("btnToggle")),
        stateDot: /** @type {HTMLElement} */ (document.getElementById("stateDot")),
        stateText: /** @type {HTMLElement} */ (document.getElementById("stateText")),
        status: /** @type {HTMLElement} */ (document.getElementById("status")),
        logDot: /** @type {HTMLElement} */ (document.getElementById("logDot")),
        logText: /** @type {HTMLElement} */ (document.getElementById("logText")),
    };

    if (!el.graph) throw new Error("Missing #graph");

    const setLogPill = createPillController({ dot: el.logDot, text: el.logText });
    const setStatePill = createPillController({ dot: el.stateDot, text: el.stateText });

    function h(tag, className, text) {
        const e = document.createElement(tag);
        if (className) e.className = className;
        if (text !== undefined) e.textContent = text;
        return e;
    }

    function safeDisconnect(node) {
        try {
            if (node && node.disconnect) node.disconnect();
        } catch (_) {}
    }

    function safeConnect(a, b) {
        try {
            a.connect(b);
        } catch (e) {
            log(`connect failed: ${e}`);
        }
    }

    function createParamRow(labelText) {
        const row = h("div", "row");
        const label = h("label", null, labelText);
        row.append(label);
        return { row, label };
    }

    function formatBytes(bytes) {
        const b = Number(bytes);
        if (!Number.isFinite(b) || b < 0) return "-";
        if (b < 1024) return `${Math.round(b)} B`;
        const kb = b / 1024;
        if (kb < 1024) return `${kb.toFixed(1)} KiB`;
        const mb = kb / 1024;
        if (mb < 1024) return `${mb.toFixed(2)} MiB`;
        const gb = mb / 1024;
        return `${gb.toFixed(2)} GiB`;
    }

    function formatBitrate(bitsPerSecond) {
        const bps = Number(bitsPerSecond);
        if (!Number.isFinite(bps) || bps <= 0) return "-";
        if (bps >= 1_000_000) return `${(bps / 1_000_000).toFixed(2)} Mbps`;
        return `${Math.round(bps / 1000)} kbps`;
    }

    function createInfoRow(labelText) {
        const row = h("div", "row");
        row.append(h("label", null, labelText));
        const value = h("div", "value", "-");
        row.append(value);
        row.append(h("div", "hint-inline", ""));
        return { row, value };
    }

    function createRangeNumberRow(options) {
        const { label, range, number, onChange, map } = options;

        const { row } = createParamRow(label);
        const rangeEl = document.createElement("input");
        rangeEl.type = "range";
        Object.assign(rangeEl, range);

        const numberEl = document.createElement("input");
        numberEl.type = "number";
        Object.assign(numberEl, number);

        row.append(rangeEl);
        row.append(numberEl);

        if (map && map.rangeToNumber && map.numberToRange) {
            rangeEl.addEventListener("input", () => {
                numberEl.value = map.rangeToNumber(rangeEl.value);
                onChange();
            });
            numberEl.addEventListener("input", () => {
                rangeEl.value = map.numberToRange(numberEl.value);
                onChange();
            });
        } else {
            syncRangeAndNumber(rangeEl, numberEl, onChange);
        }

        return { row, rangeEl, numberEl };
    }

    function createSelectRow(options) {
        const { label, optionsList, onChange } = options;
        const { row } = createParamRow(label);
        const select = document.createElement("select");

        for (const opt of optionsList) {
            const o = document.createElement("option");
            o.value = opt.value;
            o.textContent = opt.text;
            if (opt.selected) o.selected = true;
            select.append(o);
        }

        select.addEventListener("change", () => onChange(select.value));

        row.append(select);
        row.append(h("div", "hint-inline", ""));

        return { row, select };
    }

    function createCheckboxRow(options) {
        const { label, checked, onChange, hint } = options;
        const row = h("div", "row");
        row.style.gridTemplateColumns = "140px 1fr 110px";

        row.append(h("label", null, label));

        const wrap = h("div", null);
        const cbLabel = h("label", null);
        cbLabel.style.display = "flex";
        cbLabel.style.gap = "10px";
        cbLabel.style.alignItems = "center";

        const cb = document.createElement("input");
        cb.type = "checkbox";
        cb.checked = !!checked;

        cbLabel.append(cb);
        wrap.append(cbLabel);

        row.append(wrap);
        row.append(h("div", "hint-inline", hint || ""));

        cb.addEventListener("change", () => onChange(cb.checked));

        return { row, cb };
    }

    const nodeUi = {
        h,
        clampNumber,
        parseNumber,
        createInfoRow,
        createRangeNumberRow,
        createSelectRow,
        createCheckboxRow,
        safeConnect,
        safeDisconnect,
        formatBytes,
        formatBitrate,
        setupMediaElementFilePicker,
        setupStatusTextLoop,
        createTimeDomainOscilloscope,
        createFrequencyDomainSpectrum,
    };

    function createNodeCard(options) {
        const card = h("div", "panel node-card");
        card.style.boxShadow = "none";
        card.style.background = "rgba(0, 0, 0, 0.12)";

        const hd = h("div", "hd");
        hd.style.background = "transparent";

        const title = h("div", "node-title");
        title.append(h("div", null, options.title));
        if (options.subtitle) title.append(h("div", "hint-inline", options.subtitle));

        const controls = h("div", "node-controls");
        if (options.controls) controls.append(options.controls);

        hd.append(title);
        hd.append(controls);

        const bd = h("div", "bd");

        card.append(hd);
        card.append(bd);

        return { card, bd, controls };
    }

    function createAddSeparator(options) {
        const sep = h("div", "graph-sep");
        const line = h("div", "line");
        const add = h("div", "add");
        const label = h("label", null, "Add:");

        const select = document.createElement("select");
        const placeholder = document.createElement("option");
        placeholder.value = "";
        placeholder.textContent = "(select)";
        placeholder.selected = true;
        placeholder.disabled = true;
        select.append(placeholder);

        for (const opt of options.items) {
            const o = document.createElement("option");
            o.value = opt.value;
            o.textContent = opt.text;
            select.append(o);
        }

        select.addEventListener("change", () => {
            const v = select.value;
            // Reset to placeholder after each pick.
            select.selectedIndex = 0;
            options.onPick(v);
        });

        add.append(label);
        add.append(select);

        sep.append(line);
        sep.append(add);

        return { sep, select };
    }

    function createInspector(options) {
        /** @type {AudioContext|null} */
        let ctx = null;

        /** @type {boolean} */
        let topologyFrozen = false;

        /** @type {boolean} */
        let topologyDirty = true;

        // During teardown/switching we must not rebuild the graph, otherwise we can
        // accidentally create/connect new nodes for the old source and then lose
        // the ability to disconnect them.
        /** @type {boolean} */
        let suppressGraphRebuild = false;

        const processorKinds = [
            { value: "gain", text: "GainNode" },
            { value: "delay", text: "DelayNode" },
            { value: "stereoPanner", text: "StereoPannerNode" },
            { value: "biquadFilter", text: "BiquadFilterNode" },
            { value: "dynamicsCompressor", text: "DynamicsCompressorNode" },
            { value: "analyser", text: "AnalyserNode" },
            { value: "ohnoes", text: "OhNoesNode" },
        ];

        const sourceKinds = [
            { value: "oscillator", text: "OscillatorNode" },
            { value: "mediaElement", text: "MediaElementAudioSourceNode" },
            { value: "bufferSource", text: "AudioBufferSourceNode" },
            { value: "constantSource", text: "ConstantSourceNode" },
        ];

        const inspector = {
            withGraphRebuildSuppressed(fn) {
                suppressGraphRebuild = true;
                try {
                    fn();
                } finally {
                    suppressGraphRebuild = false;
                }
            },

            bindSourceGraphChanged() {
                const src = inspector.source;
                if (!src || !src.setOnGraphChanged) return;
                src.setOnGraphChanged(() => {
                    if (suppressGraphRebuild) return;
                    inspector.markTopologyDirty();
                    inspector.rebuildConnections();
                });
            },
            /** @type {ReturnType<typeof createSourceInstance>} */
            source: null,
            /** @type {Array<ReturnType<typeof createProcessorInstance>>} */
            processors: [],
            /** @type {Array<{ select: HTMLSelectElement }>} */
            addSelects: [],
            /** @type {Array<HTMLButtonElement>} */
            deleteButtons: [],

            markTopologyDirty() {
                topologyDirty = true;
            },

            clearTopologyDirty() {
                topologyDirty = false;
            },

            isTopologyDirty() {
                return topologyDirty;
            },

            render() {
                options.graph.innerHTML = "";
                inspector.addSelects = [];
                inspector.deleteButtons = [];

                // Source card
                {
                    const src = inspector.source;
                    const sourcePicker = document.createElement("select");
                    {
                        const placeholder = document.createElement("option");
                        placeholder.value = "";
                        placeholder.textContent = "Source";
                        placeholder.disabled = true;
                        placeholder.selected = true;
                        sourcePicker.append(placeholder);

                        for (const opt of sourceKinds) {
                            const o = document.createElement("option");
                            o.value = opt.value;
                            o.textContent = opt.text;
                            if (src.kind === opt.value) o.selected = true;
                            sourcePicker.append(o);
                        }
                    }

                    sourcePicker.addEventListener("change", () => {
                        inspector.trySwitchSource(sourcePicker.value);
                    });

                    const controls = h("div", null);
                    controls.style.display = "flex";
                    controls.style.alignItems = "center";
                    controls.style.gap = "10px";
                    controls.append(sourcePicker);

                    const card = createNodeCard({ title: "Source", subtitle: src.title, controls });
                    card.card.dataset.topologyControl = "1";
                    card.bd.append(src.uiRoot);

                    inspector.addSelects.push({ select: sourcePicker });

                    options.graph.append(card.card);
                }

                // Separator + processors
                for (let i = 0; i <= inspector.processors.length; i++) {
                    const sep = createAddSeparator({
                        items: processorKinds,
                        onPick: kind => inspector.tryAddProcessorAt(i, kind),
                    });

                    sep.sep.dataset.topologyControl = "1";
                    inspector.addSelects.push({ select: sep.select });
                    options.graph.append(sep.sep);

                    if (i === inspector.processors.length) break;

                    const p = inspector.processors[i];
                    const del = document.createElement("button");
                    del.textContent = "Delete";
                    del.className = "danger";
                    del.addEventListener("click", () => inspector.tryDeleteProcessorAt(i));

                    const controls = h("div", null);
                    controls.style.display = "flex";
                    controls.style.alignItems = "center";
                    controls.style.gap = "10px";
                    controls.append(del);

                    const card = createNodeCard({ title: "Node", subtitle: p.title, controls });
                    card.card.dataset.topologyControl = "1";
                    card.bd.append(p.uiRoot);

                    inspector.deleteButtons.push(del);

                    options.graph.append(card.card);
                }

                // Destination
                {
                    const card = createNodeCard({
                        title: "Destination",
                        subtitle: ctx ? "AudioDestinationNode" : "(no AudioContext)",
                    });
                    options.graph.append(card.card);
                }

                inspector.updateTopologyFrozenUI();
            },

            setContext(newCtx) {
                ctx = newCtx;
                inspector.source.setContext(ctx);
                for (const p of inspector.processors) p.setContext(ctx);
                inspector.rebuildConnections();
                inspector.render();

                const running = !!ctx && ctx.state === "running";
                for (const p of inspector.processors) {
                    if (p.setControlsDisabled) p.setControlsDisabled(running);
                }
            },

            setTopologyFrozen(frozen) {
                topologyFrozen = frozen;
                inspector.updateTopologyFrozenUI();
                inspector.updateAnalyserVisualizers();

                for (const p of inspector.processors) {
                    if (p.setControlsDisabled) p.setControlsDisabled(frozen);
                }
            },

            onContextSuspended() {
                try {
                    if (inspector.source.onContextSuspended) inspector.source.onContextSuspended();
                } catch (e) {
                    log(`onContextSuspended failed: ${e}`);
                }

                for (const p of inspector.processors) {
                    try {
                        if (p.onContextSuspended) p.onContextSuspended();
                    } catch (e) {
                        log(`processor onContextSuspended failed: ${e}`);
                    }
                }

                inspector.updateAnalyserVisualizers();
            },

            onContextResumed() {
                try {
                    if (inspector.source.onContextResumed) inspector.source.onContextResumed();
                } catch (e) {
                    log(`onContextResumed failed: ${e}`);
                }

                for (const p of inspector.processors) {
                    try {
                        if (p.onContextResumed) p.onContextResumed();
                    } catch (e) {
                        log(`processor onContextResumed failed: ${e}`);
                    }
                }

                inspector.updateAnalyserVisualizers();
            },

            updateTopologyFrozenUI() {
                const frozen = topologyFrozen;

                const topologyControls = options.graph.querySelectorAll("[data-topology-control='1']");
                for (const e of topologyControls) {
                    e.classList.toggle("topology-frozen", frozen);
                }

                for (const w of inspector.addSelects) {
                    w.select.disabled = frozen;
                }
                for (const b of inspector.deleteButtons) {
                    b.disabled = frozen;
                }
            },

            rebuildConnections() {
                if (!ctx) return;

                // Ensure audio nodes exist.
                inspector.source.ensureAudio(ctx);
                for (const p of inspector.processors) p.ensureAudio(ctx);

                // Disconnect everything we own.
                inspector.source.disconnectAll();
                for (const p of inspector.processors) p.disconnectAll();

                // Connect linear chain: source -> processors -> destination
                /** @type {AudioNode|null} */
                let out = inspector.source.outputNode();

                for (const p of inspector.processors) {
                    const node = p.audioNode();
                    if (out) safeConnect(out, node);
                    out = p.outputNode();
                }

                if (out) safeConnect(out, ctx.destination);

                inspector.updateAnalyserVisualizers();

                if (inspector.clearTopologyDirty) inspector.clearTopologyDirty();
            },

            updateAnalyserVisualizers() {
                const running = !!ctx && ctx.state === "running";
                for (const p of inspector.processors) {
                    if (p.kind === "analyser") {
                        p.setVisualizersEnabled(running);
                    }
                }
            },

            tryAddProcessorAt(index, kind) {
                if (topologyFrozen) {
                    log("Topology is frozen while running; stop first.");
                    return;
                }
                const inst = createProcessorInstance(kind, log);
                inst.setContext(ctx);
                inspector.processors.splice(index, 0, inst);
                inspector.markTopologyDirty();
                inspector.rebuildConnections();
                inspector.render();
                log(`Added ${inst.title} at ${index}`);
            },

            tryDeleteProcessorAt(index) {
                if (topologyFrozen) {
                    log("Topology is frozen while running; stop first.");
                    return;
                }
                const inst = inspector.processors[index];
                if (!inst) return;
                inst.teardown();
                inspector.processors.splice(index, 1);
                inspector.markTopologyDirty();
                inspector.rebuildConnections();
                inspector.render();
                log(`Deleted processor at ${index}`);
            },

            trySwitchSource(kind) {
                if (topologyFrozen) {
                    log("Topology is frozen while running; stop first.");
                    inspector.render();
                    return;
                }

                if (!kind || kind === inspector.source.kind) {
                    inspector.render();
                    return;
                }

                inspector.markTopologyDirty();
                inspector.withGraphRebuildSuppressed(() => {
                    inspector.teardownAudio();
                    inspector.source.teardown();
                });
                inspector.source = createSourceInstance(kind, log);
                inspector.bindSourceGraphChanged();
                inspector.source.setContext(ctx);
                inspector.rebuildConnections();
                inspector.render();
                log(`Switched source -> ${inspector.source.title}`);
            },

            teardownAudio() {
                for (const p of inspector.processors) p.teardownAudio();
                inspector.source.teardownAudio();
            },

            teardownAll() {
                inspector.teardownAudio();
                for (const p of inspector.processors) p.teardown();
                inspector.processors = [];
                inspector.source.teardown();
            },
        };

        inspector.source = createSourceInstance("oscillator", log);
        inspector.bindSourceGraphChanged();
        inspector.source.setContext(ctx);

        inspector.render();

        return inspector;
    }

    function createSourceInstance(kind, log) {
        if (kind === "oscillator") return createOscillatorSource(log, nodeUi);
        if (kind === "mediaElement") return createMediaElementSource(log, nodeUi);
        if (kind === "bufferSource") return createBufferSource(log, nodeUi);
        if (kind === "constantSource") return createConstantSource(log, nodeUi);
        return createOscillatorSource(log, nodeUi);
    }

    function createProcessorInstance(kind, log) {
        if (kind === "gain") return createGainProcessor(log, nodeUi);
        if (kind === "delay") return createDelayProcessor(log, nodeUi);
        if (kind === "stereoPanner") return createStereoPannerProcessor(log, nodeUi);
        if (kind === "biquadFilter") return createBiquadFilterProcessor(log, nodeUi);
        if (kind === "dynamicsCompressor") return createDynamicsCompressorProcessor(log, nodeUi);
        if (kind === "analyser") return createAnalyserProcessor(log, nodeUi);
        if (kind === "ohnoes") return createOhNoesProcessor(log, nodeUi);
        return createGainProcessor(log, nodeUi);
    }
    const inspector = createInspector({ graph: el.graph });

    /** @type {AudioContext|null} */
    let ctx = null;

    const statusLoop = setupStatusTextLoop({
        target: el.status,
        intervalMs: 250,
        text: () => {
            try {
                const state = ctx ? ctx.state : "uninitialized";
                const t = ctx ? ctx.currentTime.toFixed(3) : "-";
                const sr = ctx ? ctx.sampleRate : "-";
                const q = ctx ? ctx.baseLatency : "-";
                const src = inspector && inspector.source ? inspector.source.kind : "?";
                return `state=${state}  source=${src}  currentTime=${t}  sampleRate=${sr}  baseLatency=${q}`;
            } catch (_) {
                return "status unavailable";
            }
        },
    });

    function updateTransportButton() {
        const running = !!ctx && ctx.state === "running";
        el.btnToggle.textContent = running ? "Stop" : "Start";
        el.btnToggle.classList.toggle("primary", !running);
        el.btnToggle.classList.toggle("danger", running);
    }

    function updateStateUI() {
        const state = ctx ? ctx.state : "uninitialized";
        setStatePill(state, ctx && state === "running" ? true : ctx ? null : false);
    }

    async function stopContext() {
        if (!ctx) return;
        try {
            await ctx.suspend();
        } catch (e) {
            log(`ctx.suspend() failed: ${e}`);
        }

        inspector.setTopologyFrozen(ctx.state === "running");
        if (ctx.state === "suspended") inspector.onContextSuspended();
        updateStateUI();
        updateTransportButton();
    }

    async function startContext() {
        if (!ctx) return;

        // If topology changed while stopped, rebuild the graph before starting.
        // This avoids keeping any stale connections alive.
        if (inspector.isTopologyDirty && inspector.isTopologyDirty()) {
            inspector.teardownAudio();
            inspector.rebuildConnections();
            if (inspector.clearTopologyDirty) inspector.clearTopologyDirty();
        }

        try {
            await ctx.resume();
        } catch (e) {
            log(`ctx.resume() failed: ${e}`);
        }

        inspector.setTopologyFrozen(ctx.state === "running");
        if (ctx.state === "running") inspector.onContextResumed();
        updateStateUI();
        updateTransportButton();
    }

    el.btnToggle.addEventListener("click", async () => {
        if (!ctx) return;
        if (ctx.state === "running") await stopContext();
        else await startContext();
    });

    async function initOnce() {
        if (ctx) return;

        try {
            ctx = new AudioContext({ latencyHint: "interactive" });
        } catch (e) {
            ctx = null;
            log(`AudioContext creation failed: ${e}`);
            updateStateUI();
            updateTransportButton();
            return;
        }

        log(`Created AudioContext (state=${ctx.state}, sampleRate=${ctx.sampleRate})`);

        ctx.onstatechange = () => {
            log(`AudioContext statechange -> ${ctx.state}`);
            inspector.setTopologyFrozen(ctx.state === "running");
            if (ctx.state === "running") inspector.onContextResumed();
            if (ctx.state === "suspended") inspector.onContextSuspended();
            updateStateUI();
            updateTransportButton();
        };

        inspector.setContext(ctx);

        // Ensure we start in suspended state.
        await stopContext();

        statusLoop.start();

        inspector.setTopologyFrozen(ctx.state === "running");
        updateStateUI();
        updateTransportButton();
    }

    // Initial state.
    inspector.setTopologyFrozen(true);
    updateStateUI();
    updateTransportButton();
    setLogPill("ready", null);

    initOnce();
})();
