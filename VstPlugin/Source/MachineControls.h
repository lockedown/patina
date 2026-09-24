// MachineControls.h
//
// C++ port of Sources/Audio/MachineControls.swift's visibility rule for
// the VST editor: which knobs make sense to SHOW for a given machine,
// driven by the same AkzMachineProfile capability flags the Swift app
// reads (hasVariableSampleRate, filterHasResonance) rather than a
// hand-written switch per machine. Only the effect's own, smaller
// parameter set is represented here -- no Transpose/Stretch/Cycle/
// Quality/Width, all excluded from the real-time signal path (see
// Sources/Core/RealtimeChannel.h's header comment for why).

#ifndef PATINA_FX_MACHINE_CONTROLS_H
#define PATINA_FX_MACHINE_CONTROLS_H

#include "include/AkaizerCore.h"

namespace patinafx {

// Bandwidth means something on any machine whose sample rate is a
// selectable spec, not a single fixed value -- continuously variable
// machines (S900/S950, Fairlight, Mirage) get a live knob, and
// dual-fixed-rate machines (S1000/S2000/S3000/S3200) get the same knob
// snapping between their two real rates via resolveSampleRateHz. Only
// genuinely single-rate machines (SP-1200, Emulator II) hide it.
bool machineHasBandwidthControl(AkzMachine machine);

// Resonance only does anything on machines whose filter actually has
// one -- same filterHasResonance scoping as MachineControls.swift's
// Resonance descriptor.
bool machineHasResonanceControl(AkzMachine machine);

} // namespace patinafx

#endif // PATINA_FX_MACHINE_CONTROLS_H
