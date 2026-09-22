import XCTest
import SwiftTreeSitter
import TreeSitterOkf

final class TreeSitterOkfTests: XCTestCase {
    func testCanLoadGrammar() throws {
        let parser = Parser()
        let language = Language(language: tree_sitter_okf())
        XCTAssertNoThrow(try parser.setLanguage(language),
                         "Error loading OKF grammar")
    }
}
