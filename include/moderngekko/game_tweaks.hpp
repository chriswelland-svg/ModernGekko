#pragma once

// Built-in, per-game gameplay options that need to intercept guest code.
// They ship inside the runner as attached mods (no package in Mods/), are
// attached only for the exact game build they were reverse-engineered on, and
// are switched on and off live through the flags below (F1 > Gameplay).

#include "moderngekko/mod_abi.h"

#include <string>

namespace moderngekko::tweaks
{
// Beach Spikers: when both players on the receiving team are human, draw the
// ball landing marker in the neutral (COM) grey instead of the colour of the
// player the game picked, so the two players decide between themselves who
// takes the ball. The serve-receive marker keeps its colour; grey starts with
// the first touch after the serve and lasts until the next serve. Off by
// default.
void SetNeutralBallMarker(bool enabled);
bool NeutralBallMarker();

// True when this game build has a neutral ball marker implementation (the
// F1 menu only shows the option then).
bool SupportsNeutralBallMarker(const std::string& disc_id, const std::string& dol_sha256);

// The built-in tweak mod for this game build, or nullptr.
const ModernGekkoModDesc* BuiltinTweaksFor(const std::string& disc_id,
                                           const std::string& dol_sha256);
}  // namespace moderngekko::tweaks
