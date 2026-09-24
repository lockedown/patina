// MachineControls.cpp
//
// See MachineControls.h.

#include "MachineControls.h"

namespace patinafx {

bool machineHasBandwidthControl(AkzMachine machine) {
    // Any machine with a selectable rate gets the knob -- not just
    // hasVariableSampleRate (continuous) machines. On a dual-FIXED-rate
    // machine (S1000/S2000/S3000/S3200, min < max but not variable)
    // resolveSampleRateHz snaps the request to the nearer bound, so a
    // knob behaves like the hardware's two-position rate switch and
    // exposes the cited 22.05 kHz lo-fi mode. Deliberate divergence
    // from MachineControls.swift, which uses hasVariableSampleRate as
    // a knob-vs-picker hint the plugin doesn't have.
    const AkzMachineProfile* profile = akz_machine_profile(machine);
    return profile->minSampleRateHz < profile->maxSampleRateHz;
}

bool machineHasResonanceControl(AkzMachine machine) {
    return akz_machine_profile(machine)->filterHasResonance != 0;
}

} // namespace patinafx
