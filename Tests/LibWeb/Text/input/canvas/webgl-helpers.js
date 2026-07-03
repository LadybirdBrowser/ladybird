function errorName(gl, error) {
    switch (error) {
        case gl.NO_ERROR:
            return "NO_ERROR";
        case gl.INVALID_ENUM:
            return "INVALID_ENUM";
        case gl.INVALID_VALUE:
            return "INVALID_VALUE";
        case gl.INVALID_OPERATION:
            return "INVALID_OPERATION";
        case gl.OUT_OF_MEMORY:
            return "OUT_OF_MEMORY";
        default:
            return `0x${error.toString(16)}`;
    }
}

function framebufferStatusName(gl, status) {
    switch (status) {
        case gl.FRAMEBUFFER_COMPLETE:
            return "FRAMEBUFFER_COMPLETE";
        case gl.FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
            return "FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
        case gl.FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
            return "FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
        case gl.FRAMEBUFFER_UNSUPPORTED:
            return "FRAMEBUFFER_UNSUPPORTED";
        default:
            return `0x${status.toString(16)}`;
    }
}

function clearErrors(gl) {
    while (gl.getError() !== gl.NO_ERROR) {}
}

function printError(label, gl) {
    println(`${label}: ${errorName(gl, gl.getError())}`);
}
