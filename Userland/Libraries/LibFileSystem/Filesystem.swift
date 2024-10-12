import Foundation
import Glibc

struct FileSystem {
    var fs = FileManager.default
    enum PreserveMode: UInt {
        case nothing = 0
        case permissions = 1
        case ownership = 2
        case timestamps = 4
    }
    static func currentWorkingDirectory() -> String? {
        return FileManager.default.currentDirectoryPath
    }

    static func absolutePath(for path: String) -> String? {
        guard !path.isEmpty else { return nil }
        return realpath(path, nil).map { String(cString: $0) }
    }

    static func pathExists(_ path: String) -> Bool {
        var ret = false;
        FileManager.default.fileExists(atPath: path, isDirectory: ret);
        return ret;
    }

    static func isDevice(_ path: String) -> Bool {
        let attributes = try? FileManager.default.attributesOfItem(atPath: path)
        return (attributes?[.type] as? FileAttributeType == .typeBlockSpecial) || (attributes?[.type] as? FileAttributeType == .typeCharacterSpecial)
    }

    static func isBlockDevice(_ path: String) -> Bool {
        let attributes = try? FileManager.default.attributesOfItem(atPath: path)
        return attributes?[.type] as? FileAttributeType == .typeBlockSpecial
    }

    static func isCharDevice(_ path: String) -> Bool {
        let attributes = try? FileManager.default.attributesOfItem(atPath: path)
        return attributes?[.type] as? FileAttributeType == .typeCharacterSpecial
    }

    static func isRegularFile(_ path: String) -> Bool {
        let attributes = try? FileManager.default.attributesOfItem(atPath: path)
        return attributes?[.type] as? FileAttributeType == .typeRegular
    }

    static func isDirectory(_ path: String) -> Bool {
        var isDir = false
        return FileManager.default.fileExists(atPath: path, isDirectory: &isDir) && isDir
    }

    static func isLink(_ path: String) -> Bool {
        let attributes = try? FileManager.default.attributesOfItem(atPath: path)
        return attributes?[.type] as? FileAttributeType == .typeSymbolicLink
    }

    static func copyFile(from source: String, to destination: String, preserveMode: PreserveMode = .nothing) throws {
        try FileManager.default.copyItem(atPath: source, toPath: destination)
        if preserveMode != .nothing {
            try preserveAttributes(from: source, to: destination, preserveMode: preserveMode)
        }
    }

    static func copyDirectory(from source: String, to destination: String, preserveMode: PreserveMode = .nothing) throws {
        try FileManager.default.copyItem(atPath: source, toPath: destination)
        if preserveMode != .nothing {
            try preserveAttributes(from: source, to: destination, preserveMode: preserveMode)
        }
    }

    static func removeFile(_ path: String) throws {
        try FileManager.default.removeItem(atPath: path)
    }

    static func removeDirectory(_ path: String) throws {
        try FileManager.default.removeItem(atPath: path)
    }

    static func blockDeviceSize(from path: String) -> off_t? {
        let fd = open(path, O_RDONLY)
        guard fd >= 0 else { return nil }
        var size: off_t = 0
        if ioctl(fd, UInt(0x8008_1272), &size) == -1 {  // FIXME
            close(fd)
            return nil
        }
        close(fd)
        return size
    }

    static func looksLikeSharedLibrary(_ path: String) -> Bool {
        return path.hasSuffix(".so") || path.contains(".so.")
    }

    private static func preserveAttributes(from source: String, to destination: String, preserveMode: PreserveMode) throws {
        let attributes = try FileManager.default.attributesOfItem(atPath: source)

        if preserveMode.contains(.permissions), let permissions = attributes[.posixPermissions] as? NSNumber {
            try FileManager.default.setAttributes([.posixPermissions: permissions], ofItemAtPath: destination)
        }
        if preserveMode.contains(.ownership), let owner = attributes[.ownerAccountName] as? String, let group = attributes[.groupOwnerAccountName] as? String {
            try FileManager.default.setAttributes([.ownerAccountName: owner, .groupOwnerAccountName: group], ofItemAtPath: destination)
        }
        if preserveMode.contains(.timestamps), let modificationDate = attributes[.modificationDate] as? Date {
            try FileManager.default.setAttributes([.modificationDate: modificationDate], ofItemAtPath: destination)
        }
    }

    enum TempFileType {
        case directory
        case file
    }

    class TempFile {
        let path: String
        let type: TempFileType

        deinit {
            // Temporary files aren't removed by anyone else, so we must do it ourselves.
            let recursionMode: FileSystem.RecursionMode = type == .directory ? .allowed : .disallowed
            do {
                try FileSystem.remove(at: path, recursionMode: recursionMode)
            } catch {
                print("Warning: Removal of temporary file failed '\(path)': \(error.localizedDescription)")
            }
        }

        private init(type: TempFileType, path: String) {
            self.type = type
            self.path = path
        }

        static func createTempDirectory() throws -> TempFile {
            let template = "/tmp/tmp.XXXXXX"
            var templateCopy = template
            let path = try FileManager.default.createTemporaryDirectory(template: &templateCopy)
            return TempFile(type: .directory, path: path)
        }

        static func createTempFile() throws -> TempFile {
            let template = "/tmp/tmp.XXXXXX"
            var templateCopy = template
            let filePath = try FileManager.default.createTemporaryFile(template: &templateCopy)
            return TempFile(type: .file, path: filePath)
        }
    }

    func createTemporaryDirectory(template: inout String) throws -> String {
        let directoryURL = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
        try createDirectory(at: directoryURL, withIntermediateDirectories: true, attributes: nil)
        return directoryURL.path
    }

    func createTemporaryFile(template: inout String) throws -> String {
        let fileURL = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
        let fileHandle = FileHandle(forWritingAtPath: fileURL.path) ?? FileHandle(fileDescriptor: open(fileURL.path, O_CREAT | O_RDWR, 0o600))
        fileHandle.closeFile()
        return fileURL.path
    }

}
