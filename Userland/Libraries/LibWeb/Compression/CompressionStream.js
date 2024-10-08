function CompressionStream_constructor(format) {
    this.transform = new TransformStream({
        start(controller) {
            controller.temp = [];
            return Promise.resolve();
        },
        transform(chunk, controller) {
            controller.temp.push(CompressionStream.compress(format, chunk));
            return Promise.resolve();
        },
        flush(controller) {
            for (chunk of controller.temp) {
                controller.enqueue(chunk);
            }
            return Promise.resolve();
        },
    });
    this.readable = this.transform.readable;
    this.writable = this.transform.writable;
}
