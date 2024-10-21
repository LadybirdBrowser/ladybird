import Foundation

extension NSString {
    func toString() -> String {
        return self as String
    }

    func toByteString() -> Data {
        return Data(bytes: self.utf8String!, count: strlen(self.utf8String!))
    }
}

extension String {
	func toNSString() -> NSString {
		return self as NSString;
	}

	func toNSData() -> Data {
		return Data(self.utf8)
	}
}

func deserialize_json_to_dictionary(_ json: String) -> Dictionary<String, Any>? {
    guard let jsonData = json.data(using: .utf8) else { return nil }

    do {
        if let dictionary = try JSONSerialization.jsonObject(with: jsonData, options: []) as? [String: Any] {
            return dictionary
        }
    } catch {
        NSLog("Error deserializing DOM tree: \(error)")
    }

    return nil
}
