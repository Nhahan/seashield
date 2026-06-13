// Window-only screen recorder (ScreenCaptureKit): composites JUST the target
// window's layer, so OS dialogs/notifications floating above the game never
// reach the recording — unlike display capture (screencapture -v), which
// grabs whatever macOS draws on top.
//
//   swiftc -O record_window.swift -o record_window
//   ./record_window <title-or-app-substring> <seconds> <out.mov> [fps]
//
// Waits up to 60 s for the window to appear, records at most <seconds>,
// finishes early (cleanly) if the window closes.
import AVFoundation
import AppKit
import CoreMedia
import ScreenCaptureKit

// CLI tools have no NSApplication; without this, ScreenCaptureKit's first
// CoreGraphics call can race the WindowServer connection and trip
// CGS_REQUIRE_INIT (observed intermittently).
_ = NSApplication.shared

let cli = CommandLine.arguments
guard cli.count >= 4, let seconds = Double(cli[2]) else {
	FileHandle.standardError.write(
		"usage: record_window <title-substring> <seconds> <out.mov> [fps] [index]\n".data(using: .utf8)!)
	exit(2)
}
let titleNeedle = cli[1]
let outURL = URL(fileURLWithPath: cli[3])
let fps: Int32 = cli.count > 4 ? Int32(cli[4]) ?? 60 : 60
// Optional window index: when several windows share the title (e.g. multiple
// co-op clients), pick the Nth sorted left-to-right by on-screen position.
// Omitted -> largest window (the single-client default).
let windowIndex: Int? = cli.count > 5 ? Int(cli[5]) : nil

final class Recorder: NSObject, SCStreamOutput, SCStreamDelegate {
	let writer: AVAssetWriter
	let input: AVAssetWriterInput
	var started = false
	var frames = 0
	let onStop: () -> Void

	init(url: URL, width: Int, height: Int, onStop: @escaping () -> Void) throws {
		try? FileManager.default.removeItem(at: url)
		writer = try AVAssetWriter(outputURL: url, fileType: .mov)
		let settings: [String: Any] = [
			AVVideoCodecKey: AVVideoCodecType.h264,
			AVVideoWidthKey: width,
			AVVideoHeightKey: height,
			AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: 12_000_000],
		]
		input = AVAssetWriterInput(mediaType: .video, outputSettings: settings)
		input.expectsMediaDataInRealTime = true
		writer.add(input)
		self.onStop = onStop
		super.init()
		writer.startWriting()
	}

	func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer,
	            of type: SCStreamOutputType) {
		guard type == .screen, CMSampleBufferIsValid(sb) else { return }
		// Only frames marked complete carry image data (SCK also delivers
		// idle/blank status frames).
		guard
			let attachments = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false)
				as? [[SCStreamFrameInfo: Any]],
			let status = attachments.first?[.status] as? Int,
			status == SCFrameStatus.complete.rawValue
		else { return }
		if !started {
			started = true
			writer.startSession(atSourceTime: CMSampleBufferGetPresentationTimeStamp(sb))
		}
		if input.isReadyForMoreMediaData {
			input.append(sb)
			frames += 1
		}
	}

	func stream(_ stream: SCStream, didStopWithError error: Error) {
		// Window closed (game quit) — finish what we have.
		onStop()
	}

	func finish() {
		guard started else { return }
		input.markAsFinished()
		let semaphore = DispatchSemaphore(value: 0)
		writer.finishWriting { semaphore.signal() }
		// Bound the wait: when the captured window has already closed,
		// finishWriting's completion can stall — don't block forever.
		_ = semaphore.wait(timeout: .now() + 6)
	}
}

func findWindow() async throws -> SCWindow? {
	let content = try await SCShareableContent.excludingDesktopWindows(
		true, onScreenWindowsOnly: true)
	let matches = content.windows.filter { window in
		let title = window.title ?? ""
		let app = window.owningApplication?.applicationName ?? ""
		return (title.contains(titleNeedle) || app.contains(titleNeedle))
			&& window.frame.width > 400
	}
	guard let index = windowIndex else {
		return matches.max(by: { $0.frame.width * $0.frame.height < $1.frame.width * $1.frame.height })
	}
	// Deterministic left-to-right ordering so a fixed index maps to a fixed
	// on-screen client across the co-op clients (positioned via -WinX).
	let ordered = matches.sorted {
		$0.frame.origin.x != $1.frame.origin.x
			? $0.frame.origin.x < $1.frame.origin.x
			: $0.frame.origin.y < $1.frame.origin.y
	}
	// Only commit once all expected windows are up, so the index is stable.
	guard ordered.count > index else { return nil }
	return ordered[index]
}

let stopSignal = DispatchSemaphore(value: 0)

Task {
	var window: SCWindow?
	for _ in 0..<120 {
		window = try await findWindow()
		if window != nil { break }
		try await Task.sleep(nanoseconds: 500_000_000)
	}
	guard let window else {
		FileHandle.standardError.write("window not found: \(titleNeedle)\n".data(using: .utf8)!)
		// Diagnostic dump — empty titles here usually mean the Screen
		// Recording TCC permission is missing for this process tree.
		if let content = try? await SCShareableContent.excludingDesktopWindows(
			true, onScreenWindowsOnly: true)
		{
			for candidate in content.windows where candidate.frame.width > 400 {
				let app = candidate.owningApplication?.applicationName ?? "?"
				let title = candidate.title ?? ""
				FileHandle.standardError.write(
					"  [\(app)] '\(title)' \(Int(candidate.frame.width))x\(Int(candidate.frame.height))\n"
						.data(using: .utf8)!)
			}
		}
		exit(3)
	}

	let filter = SCContentFilter(desktopIndependentWindow: window)
	let config = SCStreamConfiguration()
	// Cap the output at ~1440p wide, keeping the window aspect.
	let scale = min(1.0, 2560.0 / window.frame.width)
	config.width = Int(window.frame.width * scale)
	config.height = Int(window.frame.height * scale)
	config.minimumFrameInterval = CMTime(value: 1, timescale: fps)
	config.showsCursor = false
	config.queueDepth = 8

	let recorder = try Recorder(url: outURL, width: config.width, height: config.height) {
		stopSignal.signal()
	}
	let stream = SCStream(filter: filter, configuration: config, delegate: recorder)
	try stream.addStreamOutput(
		recorder, type: .screen, sampleHandlerQueue: DispatchQueue(label: "seashield.rec"))
	try await stream.startCapture()
	print("recording \(config.width)x\(config.height) '\(window.title ?? "?")'")

	// Duration timeout OR early window-close, whichever first.
	DispatchQueue.global().asyncAfter(deadline: .now() + seconds) { stopSignal.signal() }
	// Hard watchdog: nothing in the stop/finish path may keep this process
	// alive past the deadline (finishWriting and stopCapture have both been
	// observed to stall once the captured window closes — never hang again).
	DispatchQueue.global().asyncAfter(deadline: .now() + seconds + 12) {
		FileHandle.standardError.write("watchdog: forcing exit\n".data(using: .utf8)!)
		exit(0)
	}
	stopSignal.wait()
	try? await stream.stopCapture()
	recorder.finish()
	print("done: \(recorder.frames) frames -> \(outURL.path)")
	exit(0)
}

RunLoop.main.run()
