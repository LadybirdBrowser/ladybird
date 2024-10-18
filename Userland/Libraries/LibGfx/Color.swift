/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import AppKit
@_exported import GfxCxx

// FIXME: Do this without extending String with an index operation that was explicitly deleted :^)
extension Swift.String {
    subscript(_ index: Int) -> Character {
        return self[self.index(self.startIndex, offsetBy: index)]
    }
}

private func hexNibbleToUInt8(_ nibble: Character) -> UInt8? {
    guard nibble.isHexDigit else {
        return nil
    }
    return UInt8(nibble.hexDigitValue!)
}

private func hexNibblesToUInt8(_ nib1: Character, _ nib2: Character) -> UInt8? {
    guard let n1 = hexNibbleToUInt8(nib1) else {
        return nil
    }
    guard let n2 = hexNibbleToUInt8(nib2) else {
        return nil
    }
    return n1 << 4 | n2
}

// FIXME: Return Gfx.Color? When swift ABI bug is fixed
public func parseHexString(_ rawString: AK.StringView) -> [Gfx.Color] {
    guard let string = Swift.String(akStringView: rawString) else {
        return []
    }

    assert(string.hasPrefix("#"))

    switch string.count {
    case 4:
        let r = hexNibbleToUInt8(string[1])
        let g = hexNibbleToUInt8(string[2])
        let b = hexNibbleToUInt8(string[3])

        guard r != nil && g != nil && b != nil else {
            return []
        }

        return [Gfx.Color(r! * 17, g! * 17, b! * 17)]
    case 5:
        let r = hexNibbleToUInt8(string[1])
        let g = hexNibbleToUInt8(string[2])
        let b = hexNibbleToUInt8(string[3])
        let a = hexNibbleToUInt8(string[4])

        guard r != nil && g != nil && b != nil && a != nil else {
            return []
        }

        return [Gfx.Color(r! * 17, g! * 17, b! * 17, a! * 17)]
    case 6: return []
    case 7:
        let r = hexNibblesToUInt8(string[1], string[2])
        let g = hexNibblesToUInt8(string[3], string[4])
        let b = hexNibblesToUInt8(string[5], string[6])

        guard r != nil && g != nil && b != nil else {
            return []
        }

        return [Gfx.Color(r!, g!, b!, UInt8(255))]
    case 8: return []
    case 9:
        let r = hexNibblesToUInt8(string[1], string[2])
        let g = hexNibblesToUInt8(string[3], string[4])
        let b = hexNibblesToUInt8(string[5], string[6])
        let a = hexNibblesToUInt8(string[7], string[8])

        guard r != nil && g != nil && b != nil && a != nil else {
            return []
        }

        return [Gfx.Color(r!, g!, b!, a!)]
    default: return []
    }
}

extension Gfx.Color {
    init(_ col: NSColor) {
        guard let rgbColor = color.usingColorSpace(.genericRGB) else {
            return nil
        }

        return GfxColor(
            red: UInt8(rgbColor.redComponent * 255),
            green: UInt8(rgbColor.greenComponent * 255),
            blue: UInt8(rgbColor.blueComponent * 255),
            alpha: UInt8(rgbColor.alphaComponent * 255)
        )
    }
}

extension NSColor {
    init(_ gfxC: Gfx.Color) {
        self = NSColor(
            red: CGFloat(color.red) / 255.0,
            green: CGFloat(color.green) / 255.0,
            blue: CGFloat(color.blue) / 255.0,
            alpha: CGFloat(color.alpha) / 255.0
        )
    }
}
