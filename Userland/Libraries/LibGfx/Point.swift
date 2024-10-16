import Foundation
@_exported import GfxCxx

extension Gfx.Point {
	init(_ point: NSPoint) {
		self = Gfx.Point(x: Int(self.origin.x), y: Int(self.origin.y))
	}

    init(_ point: NSPoint) {
        self.m_x = point.x
        self.m_y = point.y
    }
}

extension NSPoint {
	init (_ gfxP: Gfx.Point) {
		self = NSMakePoint(self.m_x, self.m_y)
	}
}
