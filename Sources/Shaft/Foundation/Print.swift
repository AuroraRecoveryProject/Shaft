// Copyright 2024 The Shaft Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

#if canImport(Rainbow)
import Rainbow
#endif

/// Prints a formatted message with file, function, and line information.
///
/// This function is used for logging and debugging purposes. It prints a
/// message with the current time, file path, function name, and line number.
/// The message is colored and formatted for easy reading.
public func mark(
    _ message: Any...,
    file: String = #file,
    function: String = #function,
    line: Int = #line
) {
    let message = message.map { "\($0)" }.joined(separator: " ")
    // time in 19:00:00.000 format
    let formatter = DateFormatter()
    formatter.dateFormat = "HH:mm:ss.SSS"
    let time = formatter.string(from: Date())
    // let filename = file.split(separator: "/").last ?? ""
    let relativePath = file.replacingOccurrences(
        of: FileManager.default.currentDirectoryPath + "/Sources/",
        with: ""
    )
    let fileinfo = "[\(relativePath):\(line)]"
    // Recovery builds omit Rainbow to keep the dependency set small, so keep
    // the colored output only when the module is available.
    #if canImport(Rainbow)
    print("\("INFO".green) \(time.cyan) \(fileinfo.magenta) \(function.yellow): \(message)")
    #else
    print("INFO \(time) \(fileinfo) \(function): \(message)")
    #endif
}
