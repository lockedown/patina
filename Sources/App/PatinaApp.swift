// PatinaApp.swift
//
// App entry point. See ContentView.swift for what's actually implemented.
//
// AppDelegate exists for one reason: a plain SwiftUI App run as a bare
// Swift Package executable (no .xcodeproj, no proper .app bundle -- see
// README's "Why not AVAudioFile" section for the project's general
// stance on avoiding framework surprises) launches with
// NSApplication.ActivationPolicy.prohibited by default. That means no
// window, no Dock icon, no menu bar -- confirmed by querying
// NSWorkspace.runningApplications while investigating why the app
// appeared to hang with nothing on screen. A proper .app bundle (Xcode
// build, or `swift package` bundling) gets the right policy from
// LaunchServices automatically; running the raw binary via `swift run`
// or directly does not. Setting the policy explicitly here fixes both.

import AppKit
import SwiftUI

/// Bridges "is there processed audio ContentView hasn't saved yet" to
/// AppDelegate, which needs to know that synchronously inside
/// `applicationShouldTerminate` -- AppDelegate is a plain NSObject, not a
/// SwiftUI View, so it can't read ContentView's state directly. See
/// ContentView's `hasUnsavedProcessedAudio` (a plain proxy onto this, not
/// its own @State) for the write side.
@MainActor
final class QuitGuard {
    static let shared = QuitGuard()
    var hasUnsavedProcessedAudio = false
}

/// Gates the RED window-close button (and Cmd+W) behind the same
/// "unsaved processed audio" check `applicationShouldTerminate` uses for
/// Cmd+Q -- and NOT via `applicationShouldTerminateAfterLastWindowClosed`,
/// which is where 2.3.2 first put it. That was too late: the window had
/// already closed by the time that check ran (it only fires AFTER the
/// last window is gone), so Cancel had nothing left to cancel -- the
/// window was already closed, leaving a windowless-but-running app with
/// no obvious way back to one, which read as "Cancel does nothing, you
/// can only actually quit." `windowShouldClose` runs BEFORE the window
/// closes, the same hook every other Mac app's "save changes?" dialog
/// uses -- Cancel here just leaves the window open, nothing else happens.
///
/// SwiftUI's WindowGroup gives no direct hook for this, hence the small
/// NSViewRepresentable below that reaches into the real NSWindow and sets
/// its delegate by hand -- a standard, if unglamorous, SwiftUI/AppKit
/// bridge for exactly this gap.
final class QuitAwareWindowDelegate: NSObject, NSWindowDelegate {
    static let shared = QuitAwareWindowDelegate()

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        guard QuitGuard.shared.hasUnsavedProcessedAudio else { return true }
        let alert = NSAlert()
        alert.messageText = "Close without saving processed audio?"
        alert.informativeText = "There's a processed render that hasn't been saved to disk yet. Closing now will discard it."
        alert.alertStyle = .warning
        alert.addButton(withTitle: "Close Anyway")
        alert.addButton(withTitle: "Cancel")
        let response = alert.runModal()
        let shouldClose = response == .alertFirstButtonReturn
        if shouldClose {
            // Confirmed the discard right here -- clear the flag so
            // applicationShouldTerminate (which fires right after, via
            // applicationShouldTerminateAfterLastWindowClosed, since this
            // IS the last window) doesn't immediately ask AGAIN about the
            // exact same already-discarded render.
            QuitGuard.shared.hasUnsavedProcessedAudio = false
        }
        return shouldClose
    }
}

/// Reaches into the real NSWindow behind ContentView (SwiftUI's
/// WindowGroup gives no other way to install a window delegate) and
/// installs QuitAwareWindowDelegate on it. The window doesn't exist yet
/// at makeNSView time, hence the deferred lookup.
private struct WindowCloseGate: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView {
        let view = NSView(frame: .zero)
        DispatchQueue.main.async { [weak view] in
            view?.window?.delegate = QuitAwareWindowDelegate.shared
        }
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) {}
}

extension View {
    /// See WindowCloseGate/QuitAwareWindowDelegate above.
    func withQuitAwareWindowClose() -> some View {
        background(WindowCloseGate())
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        // 2.3 feedback: "app doesn't start in the foreground." Diagnosed
        // via debug logging (since removed) down to the actual launch
        // method: launched via Finder/Dock/`open`, this works fine (once
        // deferred one run-loop tick, below, so the window exists before
        // anything tries to raise it). Launched by executing the binary
        // path directly from a shell instead, it doesn't -- confirmed a
        // separate, OS-level issue, not fixable from in here: a GUI
        // process launched that way is a child of the launching
        // terminal's own process, which keeps contesting it for window-
        // server focus regardless of how hard this app calls activate().
        // Not chasing that further; launch the built app normally.
        DispatchQueue.main.async {
            // ignoringOtherApps: true is documented as having no effect
            // as of macOS 14 (Sonoma tightened unsolicited self-
            // activation as an anti-focus-stealing measure) -- this app's
            // deployment target is already .v14 (Package.swift), so also
            // call the macOS-14+ no-argument replacement and force the
            // window forward regardless of app-activation state, as two
            // more independent attempts rather than trusting either one.
            NSApp.activate(ignoringOtherApps: true)
            NSApp.activate()
            NSApp.windows.first?.orderFrontRegardless()
            NSApp.windows.first?.makeKeyAndOrderFront(nil)
        }
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        NSApp.windows.first?.makeKeyAndOrderFront(nil)
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard QuitGuard.shared.hasUnsavedProcessedAudio else { return .terminateNow }

        // This is the Cmd+Q path specifically -- the window is still open
        // when this fires (Cmd+Q doesn't close it first), so Cancel here
        // leaves everything exactly as it was, no separate recovery
        // needed. The RED-BUTTON/Cmd+W close path is gated earlier, by
        // QuitAwareWindowDelegate's windowShouldClose below -- see that
        // type's comment for why closing the window used to run THIS
        // exact check too late (after the window had already closed) to
        // let Cancel mean anything.
        let alert = NSAlert()
        alert.messageText = "Quit without saving processed audio?"
        alert.informativeText = "There's a processed render that hasn't been saved to disk yet. Quitting now will discard it."
        alert.alertStyle = .warning
        alert.addButton(withTitle: "Quit Anyway")
        alert.addButton(withTitle: "Cancel")
        let response = alert.runModal()
        let shouldQuit = response == .alertFirstButtonReturn
        return shouldQuit ? .terminateNow : .terminateCancel
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        // 2.3.2 feedback: preferred over the old behaviour (app stayed
        // running, menu bar only, no window, after closing the window) --
        // this is a single-window app with nothing useful to do with no
        // window open.
        return true
    }
}

@main
struct PatinaApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        // 2.3 feedback: a first launch (no restored window frame yet) opened
        // at ContentView's own 720x600 *minimum*, which is too narrow for the
        // sidebar (220pt) plus a full 7-knob row (INTELLIGENT mode on a
        // resonance-capable machine: Transpose/Stretch/Cycle/Cutoff/
        // Resonance/Quality/Width, 72pt each) -- the knob row got clipped
        // until the user manually resized wider. Sized to fit that worst
        // case plus the LCD readout and a loaded waveform with no
        // scrolling. Doesn't touch the 720x600 floor in
        // ContentView.swift's .frame(minWidth:minHeight:) -- that's the
        // resize floor, a separate, already-correct constraint.
        .defaultSize(width: 960, height: 760)
        // Parameter undo/redo (see ParamSnapshot, ContentView's
        // undoStack/redoStack) -- replacing the system Undo/Redo item
        // rather than adding a separate one, since this app has no other
        // undo to speak of. See EditCommands.swift for the
        // ParamEditMenuCommands/focusedSceneValue bridge.
        .commands {
            ParamEditMenuCommands()
        }
    }
}
