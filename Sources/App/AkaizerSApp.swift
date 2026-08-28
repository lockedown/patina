// AkaizerSApp.swift
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
        NSApp.activate(ignoringOtherApps: true)
    }
}

@main
struct AkaizerSApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
