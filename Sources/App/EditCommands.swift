// EditCommands.swift
//
// Bridges ContentView's undo/redo (parameters only -- see ParamSnapshot
// and ContentView's undoStack/redoStack) to a real Edit menu via
// focusedSceneValue/FocusedValue, since ContentView is a plain @State
// struct with no ObservableObject a Scene-level menu could otherwise
// reach into directly.
//
// Equatable compares only the two flags, never the closures -- closures
// aren't Equatable, and comparing by flag is the right notion of
// "changed" here anyway: the closures' captured @State storage is
// stable across view updates, only whether undo/redo are currently
// possible changes.

import SwiftUI

struct ParamEditCommands: Equatable {
    var canUndo: Bool
    var canRedo: Bool
    var undo: () -> Void
    var redo: () -> Void

    static func == (a: ParamEditCommands, b: ParamEditCommands) -> Bool {
        a.canUndo == b.canUndo && a.canRedo == b.canRedo
    }
}

private struct ParamEditCommandsKey: FocusedValueKey {
    typealias Value = ParamEditCommands
}

extension FocusedValues {
    var paramEdit: ParamEditCommands? {
        get { self[ParamEditCommandsKey.self] }
        set { self[ParamEditCommandsKey.self] = newValue }
    }
}

/// @FocusedValue only resolves inside a View or a Commands conformer,
/// not directly in an App's body -- this is that conformer, referenced
/// from AkaizerSApp's `.commands { ParamEditMenuCommands() }`.
struct ParamEditMenuCommands: Commands {
    @FocusedValue(\.paramEdit) private var paramEdit

    var body: some Commands {
        CommandGroup(replacing: .undoRedo) {
            Button("Undo") { paramEdit?.undo() }
                .keyboardShortcut("z", modifiers: .command)
                .disabled(paramEdit?.canUndo != true)
            Button("Redo") { paramEdit?.redo() }
                .keyboardShortcut("z", modifiers: [.command, .shift])
                .disabled(paramEdit?.canRedo != true)
        }
    }
}
