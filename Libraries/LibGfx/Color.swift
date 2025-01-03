/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */ 

import AK
@_exported import GfxCxx

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
        guard let r = hexNibbleToUInt8(string[string.index(string.startIndex, offsetBy: 1)]),
              let g = hexNibbleToUInt8(string[string.index(string.startIndex, offsetBy: 2)]),
              let b = hexNibbleToUInt8(string[string.index(string.startIndex, offsetBy: 3)]) else {
            return []
        }
        return [Gfx.Color(r * 17, g * 17, b * 17)]
        
    case 5:
        guard let r = hexNibbleToUInt8(string[string.index(string.startIndex, offsetBy: 1)]),
              let g = hexNibbleToUInt8(string[string.index(string.startIndex, offsetBy: 2)]),
              let b = hexNibbleToUInt8(string[string.index(string.startIndex, offsetBy: 3)]),
              let a = hexNibbleToUInt8(string[string.index(string.startIndex, offsetBy: 4)]) else {
            return []
        }
        return [Gfx.Color(r * 17, g * 17, b * 17, a * 17)]
        
    case 6: 
        return [] 
        
    case 7:
        guard let r = hexNibblesToUInt8(string[string.index(string.startIndex, offsetBy: 1)],
                                        string[string.index(string.startIndex, offsetBy: 2)]),
              let g = hexNibblesToUInt8(string[string.index(string.startIndex, offsetBy: 3)],
                                        string[string.index(string.startIndex, offsetBy: 4)]),
              let b = hexNibblesToUInt8(string[string.index(string.startIndex, offsetBy: 5)],
                                        string[string.index(string.startIndex, offsetBy: 6)]) else {
            return []
        }
        return [Gfx.Color(r, g, b, 255)]
        
    case 8: 
        return [] 
        
    case 9:
        guard let r = hexNibblesToUInt8(string[string.index(string.startIndex, offsetBy: 1)],
                                        string[string.index(string.startIndex, offsetBy: 2)]),
              let g = hexNibblesToUInt8(string[string.index(string.startIndex, offsetBy: 3)],
                                        string[string.index(string.startIndex, offsetBy: 4)]),
              let b = hexNibblesToUInt8(string[string.index(string.startIndex, offsetBy: 5)],
                                        string[string.index(string.startIndex, offsetBy: 6)]),
              let a = hexNibblesToUInt8(string[string.index(string.startIndex, offsetBy: 7)],
                                        string[string.index(string.startIndex, offsetBy: 8)]) else {
            return []
        }
        return [Gfx.Color(r, g, b, a)]
        
    default: 
        return []
    }
}