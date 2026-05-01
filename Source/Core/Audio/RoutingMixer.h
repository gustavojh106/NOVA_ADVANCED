#pragma once

#include "../Constants.h"

namespace Nova
{
namespace Audio
{
    class RoutingMixer
    {
    public:
        struct LineInput
        {
            float gain = 1.0f;
            float pan = 0.0f;
            float width = 1.0f;
        };

        struct LineTarget
        {
            float gain = 1.0f;
            float pan = 0.0f;
            float width = 1.0f;
            bool muted = false;
        };

        struct Targets
        {
            LineTarget lineA;
            LineTarget lineB;
        };

        static Targets makeTargets(Nova::SwitcherMode mode,
            const LineInput& lineA,
            const LineInput& lineB) noexcept
        {
            const bool muteA = (mode == Nova::SwitcherMode::LineB_Only);
            const bool muteB = (mode == Nova::SwitcherMode::LineA_Only);
            const bool dualParallel = (mode == Nova::SwitcherMode::Dual_Parallel);

            Targets targets;
            targets.lineA = makeLineTarget(lineA, muteA, dualParallel);
            targets.lineB = makeLineTarget(lineB, muteB, dualParallel);
            return targets;
        }

        static float dualParallelCompensation() noexcept
        {
            return 0.5f;
        }

    private:
        static LineTarget makeLineTarget(const LineInput& input, bool muted, bool dualParallel) noexcept
        {
            LineTarget target;
            target.pan = input.pan;
            target.width = input.width;
            target.muted = muted;

            if (muted)
            {
                target.gain = 0.0f;
                return target;
            }

            target.gain = input.gain <= 0.001f ? 1.0f : input.gain;

            if (dualParallel)
                target.gain *= dualParallelCompensation();

            return target;
        }
    };
}
}
