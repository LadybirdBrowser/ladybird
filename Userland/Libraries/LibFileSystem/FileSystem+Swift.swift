@_exported import FileSystemCxx
import Foundation
import Glibc

// import Darwin

struct FileSystem {
    enum PreserveMode: UInt {
        case nothing = 0
        case permissions = 1
        case ownership = 2
        case timestamps = 4
    }
    enum RecursionMode {
        case Allowed
        case Disallowed
    }

    enum LinkMode {
        case Allowed
        case Disallowed
    }

    enum AddDuplicateFileMarker {
        case Yes
        case No
    }
    static func current_working_directory() -> String? {
        return FileManager.default.currentDirectoryPath
    }

    static func absolute_path(_ path: String) -> String? {
        guard !path.isEmpty else { return nil }
        return realpath(path, nil).map { String(cString: $0) }
    }

    static func exists(_ path: String) -> Bool {
        var st = stat();
        let temp = fstat(0, &st);
        return temp >= 0
    }

    static func is_device(_ path: String) -> Bool {
        let attributes = try? FileManager.default.attributesOfItem(atPath: path)
        return (attributes?[.type] as? FileAttributeType == .typeBlockSpecial) || (attributes?[.type] as? FileAttributeType == .typeCharacterSpecial)
    }

    static func is_block_device(_ path: String) -> Bool {
        let attributes = try? FileManager.default.attributesOfItem(atPath: path)
        return attributes?[.type] as? FileAttributeType == .typeBlockSpecial
    }

    static func is_char_device(_ path: String) -> Bool {
        let attributes = try? FileManager.default.attributesOfItem(atPath: path)
        return attributes?[.type] as? FileAttributeType == .typeCharacterSpecial
    }

    static func is_regular_file(_ path: String) -> Bool {
        let attributes = try? FileManager.default.attributesOfItem(atPath: path)
        return attributes?[.type] as? FileAttributeType == .typeRegular
    }

    static func is_directory(_ path: String) -> Bool {
        var isDir = false
        return FileManager.default.fileExists(atPath: path, isDirectory: &isDir) && isDir
    }

    static func is_link(_ path: String) -> Bool {
        let attributes = try? FileManager.default.attributesOfItem(atPath: path)
        return attributes?[.type] as? FileAttributeType == .typeSymbolicLink
    }

    static func remove_file(_ path: String) throws {
        try FileManager.default.removeItem(atPath: path)
    }

    static func remove_directory(_ path: String) throws {
        try FileManager.default.removeItem(atPath: path)
    }

    static func block_device_size(from path: String) -> off_t? {
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

    static func looks_like_shared_library(_ path: String) -> Bool {
        return path.hasSuffix(".so") || path.contains(".so.")
    }

    enum TempFileType {
        case directory
        case file
    }

}
