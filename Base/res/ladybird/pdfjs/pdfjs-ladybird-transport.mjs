async function openPdf(response) {
    await window.PDFViewerApplication.initializedPromise;

    const workerSrc = "resource://ladybird/pdfjs/build/pdf.worker.mjs";
    window.PDFViewerApplicationOptions.set("workerSrc", workerSrc);

    const sourceUrl = response.url;
    if (sourceUrl) window.PDFViewerApplication.setTitleUsingUrl(sourceUrl, sourceUrl);

    const contentLength = parseInt(response.headers.get("Content-Length") ?? "0", 10);
    const { PDFDataRangeTransport } = globalThis.pdfjsLib;
    const transport = new PDFDataRangeTransport(contentLength, new Uint8Array());
    window.PDFViewerApplication.open({ range: transport, disableRange: true });
    if (response.body) {
        const reader = response.body.getReader();
        try {
            for (;;) {
                const { done, value } = await reader.read();
                if (done) break;
                transport.onDataProgressiveRead(value);
            }
        } finally {
            transport.onDataProgressiveDone();
        }
    } else {
        const buf = await response.arrayBuffer();
        transport.onDataProgressiveRead(new Uint8Array(buf));
        transport.onDataProgressiveDone();
    }
}
document.addEventListener("ladybirdpdf", ({ detail: response }) => openPdf(response), { once: true });
document.dispatchEvent(new CustomEvent("ladybirdviewerready"));
