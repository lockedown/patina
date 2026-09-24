// FilterModel.cpp
//
// See FilterModel.h for the per-machine rationale. The IFilterStage
// hierarchy and makeFilterStage() factory this file dispatches to now
// live in FilterStages.h -- hoisted out so Sources/Core/RealtimeChannel.h
// (the real-time signal path, VstPlugin's reason for existing) can hold
// the same stage objects alive across calls instead of only ever using
// them the way applyFilter() below still does: build a fresh stack,
// process the whole buffer, discard. This file's own behaviour is
// unchanged -- see Tests/CoreTests/FilterModelTests.cpp.

#include "FilterModel.h"
#include "FilterStages.h"
#include "MachineProfile.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace akz {

void applyFilter(float* buffer, size_t count, AkzMachine machine, float cutoff01, float resonance01, double sampleRateHz, double transposeRatio) {
    if (count == 0) return;

    const AkzMachineProfile& profile = machineProfile(machine);
    const double trackedRatio = profile.filterTracksPitch ? transposeRatio : 1.0;
    const double cutoffHz = filterStagesMapCutoff01ToHz(cutoff01, sampleRateHz) * trackedRatio;
    const int resonanceCode = static_cast<int>(std::lround(std::max(0.0f, std::min(1.0f, resonance01)) * 15.0f));
    const int poles = std::max(1, static_cast<int>(std::lround(profile.filterSlopeDbPerOctave / 6.0)));

    // filterStageCount replaces the old ">= 24.0 dB/oct" heuristic for
    // S3200's second series SVF stage -- see AkaizerCore.h. Generalises
    // for free to any future machine needing N stages of the same
    // topology in series, not just "one or two."
    const int stageCount = std::max(1, profile.filterStageCount);
    std::vector<std::unique_ptr<IFilterStage>> stages;
    stages.reserve(static_cast<size_t>(stageCount));
    for (int i = 0; i < stageCount; ++i) {
        stages.push_back(makeFilterStage(profile.filterTopology, cutoffHz, resonanceCode, sampleRateHz, poles, profile.filterResonanceCompensation01));
    }

    for (auto& stage : stages) {
        stage->processBlock(buffer, count);
    }
}

} // namespace akz
