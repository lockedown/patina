// MachineControls.cpp
//
// See MachineControls.h.

#include "MachineControls.h"

namespace patinafx {

bool machineHasBandwidthControl(AkzMachine machine) {
    return akz_machine_profile(machine)->hasVariableSampleRate != 0;
}

bool machineHasResonanceControl(AkzMachine machine) {
    return akz_machine_profile(machine)->filterHasResonance != 0;
}

} // namespace patinafx
