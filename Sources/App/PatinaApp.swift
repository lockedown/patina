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

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        // 2.3 feedback: "app doesn't start in the foreground" -- calling
        // activate() synchronously, right here, was too early: this fires
        // before the WindowGroup's window has actually been created and
        // registered with the window server, so there was nothing yet
        // for activate to usefully raise. Deferring one run-loop tick
        // (so the window exists first) plus a second call from
        // applicationDidBecomeActive as a backstop is the standard fix
        // for this exact "launched, but some other app kept focus"
        // symptom on a fresh (non-restored) launch.
        DispatchQueue.main.async {
            NSApp.activate(ignoringOtherApps: true)
            NSApp.windows.first?.makeKeyAndOrderFront(nil)
        }
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        NSApp.windows.first?.makeKeyAndOrderFront(nil)
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
