// MachineProfile.h
//
// Internal C++ accessors for the machine profile table. AkaizerCore.h
// exposes the C-compatible akz_machine_profile(); this header exists so
// other .cpp files in Core/ can pull the same data without going through
// the C boundary.

#ifndef AKAIZER_MACHINE_PROFILE_H
#define AKAIZER_MACHINE_PROFILE_H

#include "include/AkaizerCore.h"

namespace akz {

// Returns the static profile for a machine. Never null, never needs freeing.
const AkzMachineProfile& machineProfile(AkzMachine machine);

} // namespace akz

#endif // AKAIZER_MACHINE_PROFILE_H
