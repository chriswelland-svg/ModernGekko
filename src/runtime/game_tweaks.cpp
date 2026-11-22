#include "moderngekko/game_tweaks.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Beach Spikers (USA, GBSE8P) neutral ball marker.
//
// How the game colours the marker (reverse-engineered on the DOL below):
//  - 0x8004E718 picks the receiving team's designated player and returns a
//    colour index: 0-3 = that player's controller (1P..4P), 4 = COM.
//    With two humans on a team the pick is just "whoever is nearer"; nothing
//    stops the other player from taking the ball. The index is written to
//    both marker slots (0x801E95D8+2 and +272+2) every frame.
//  - The marker draw routine (0x8002648C) queues models 21-25; models 22, 23
//    and 25 get setter[index] from the table at 0x8017F020 as a colour
//    callback. The render pass later calls the callback through a function
//    pointer with r3 = the model, and it writes the colour into every
//    material of that model:
//      0x80024ED8 1P red   0x80024F9C 2P blue   0x80025060 3P orange
//      0x80025128 4P green 0x800251EC COM grey (100,100,100)
//  - The same setters colour the 1P/2P arrows over the players (model 27...)
//    and on the sand, which must keep their colours.
//  - Model id -> model: bank 0x8020F0C8 + (id >> 9) * 40, then
//    *(*(*(bank + 12) + 8) + (id & 0x1FF) * 8) (as the game's draw does).
//  - Teams: 0x8021D018 holds 2 teams x 2 player pointers; player+28 is
//    nonzero for COM, player+37 is the controller colour index.
//
// The patch sits on the four player setters. Indirect calls always come back
// through the chassis, so the patch fires in chained builds and does not need
// the code chunk on the fallback CPU (MODERNGEKKO_MOD_PATCH_INDIRECT): no
// direct `bl` to any of them exists in the DOL. When the option is on, the
// model is one of the marker models, and the player the colour belongs to has
// a human teammate, the call is sent to the COM grey setter instead.
// Otherwise the original setter runs: the patch performs its first
// instruction (stwu r1,-16(r1)) and resumes at the next one, because jumping
// to the patched address would re-enter the patch.

namespace moderngekko::tweaks
{
namespace
{
std::atomic<bool> s_neutral_ball_marker{false};

constexpr const char* kBeachSpikersUsaDolSha256 =
    "b334c1f19aa10a218775f21e2b5d1f59cc2a925e17dd43d31e4db6183781e6a6";

constexpr std::uint32_t kGreySetter = 0x800251ECu;
constexpr std::uint32_t kModelBanks = 0x8020F0C8u;      // 40 bytes per bank
constexpr std::uint32_t kMarkerModels[] = {22u, 23u, 25u};  // the coloured ones
constexpr std::uint32_t kTeamTable = 0x8021D018u;  // 2 teams x 2 player pointers
constexpr std::uint32_t kPlayerIsCom = 28;         // u8, nonzero = COM-controlled
constexpr std::uint32_t kPlayerColour = 37;        // s8, 0-3 = controller (1P..4P)
constexpr std::uint32_t kStwuR1Minus16 = 0x9421FFF0u;

std::uint32_t Read32(CPUState* state, std::uint32_t address)
{
  return static_cast<std::uint32_t>(moderngekko_mod_read(state, address, 4));
}

std::uint8_t Read8(CPUState* state, std::uint32_t address)
{
  return static_cast<std::uint8_t>(moderngekko_mod_read(state, address, 1));
}

bool IsRamPointer(std::uint32_t address)
{
  return address >= 0x80000000u && address < 0x81800000u;
}

// The game's own model-id lookup (0x8004A158), or 0 when the bank is empty.
std::uint32_t ModelFor(CPUState* state, std::uint32_t id)
{
  const std::uint32_t bank = kModelBanks + ((id >> 9) & 0x7Fu) * 40u;
  const std::uint32_t header = Read32(state, bank + 12);
  if (!IsRamPointer(header))
    return 0;
  const std::uint32_t table = Read32(state, header + 8);
  if (!IsRamPointer(table))
    return 0;
  return Read32(state, table + (id & 0x1FFu) * 8u);
}

// Is the model being coloured (r3) one of the ball marker's models?
bool IsMarkerModel(CPUState* state)
{
  const std::uint32_t model = state->gpr[3];
  if (!IsRamPointer(model))
    return false;
  for (const std::uint32_t id : kMarkerModels)
  {
    if (ModelFor(state, id) == model)
      return true;
  }
  return false;
}

// Is the human player with this colour index on a team of two humans?
bool OnAllHumanTeam(CPUState* state, int colour)
{
  for (std::uint32_t team = 0; team < 2; ++team)
  {
    const std::uint32_t players[2] = {Read32(state, kTeamTable + team * 8),
                                      Read32(state, kTeamTable + team * 8 + 4)};
    if (!IsRamPointer(players[0]) || !IsRamPointer(players[1]))
      continue;
    bool ours = false;
    bool all_human = true;
    for (const std::uint32_t player : players)
    {
      const bool com = Read8(state, player + kPlayerIsCom) != 0;
      if (com)
        all_human = false;
      else if (static_cast<std::int8_t>(Read8(state, player + kPlayerColour)) == colour)
        ours = true;
    }
    if (ours)
      return all_human;
  }
  return false;
}

// MODERNGEKKO_TWEAKS_LOG=1 prints the first decisions (debugging aid).
bool LogDecisions()
{
  static const bool enabled = [] {
    const char* value = std::getenv("MODERNGEKKO_TWEAKS_LOG");
    return value && value[0] && value[0] != '0';
  }();
  return enabled;
}

template <std::uint32_t Setter, int Colour>
void PlayerColourSetter(CPUState* state)
{
  const bool enabled = s_neutral_ball_marker.load(std::memory_order_relaxed);
  const bool marker = enabled && IsMarkerModel(state);
  const bool grey = marker && OnAllHumanTeam(state, Colour);
  if (LogDecisions())
  {
    static int logged = 0;
    static unsigned calls = 0, marker_calls = 0, greyed = 0;
    ++calls;
    marker_calls += marker ? 1u : 0u;
    greyed += grey ? 1u : 0u;
    if (logged < 8 || (marker && logged < 40))
    {
      ++logged;
      std::fprintf(stderr, "[tweaks] setter %dP model=%08X on=%d marker=%d grey=%d\n",
                   Colour + 1, state->gpr[3], enabled, marker, grey);
    }
    if ((calls % 1200u) == 0u)
      std::fprintf(stderr, "[tweaks] %u setter calls: %u marker, %u greyed\n", calls,
                   marker_calls, greyed);
  }
  if (grey)
  {
    // Tail call: lr still points at the routine that called the setter.
    state->pc = kGreySetter;
    return;
  }
  // Run the original setter. Its first instruction is stwu r1,-16(r1);
  // checked once, so a different build can never take this path blind.
  static const bool prologue_ok = Read32(state, Setter) == kStwuR1Minus16;
  if (!prologue_ok)
  {
    static bool warned = false;
    if (!warned)
    {
      warned = true;
      std::fprintf(stderr, "[tweaks] unexpected prologue at %08X; colours left unchanged\n",
                   Setter);
    }
    // Not the code this was written against (BuiltinTweaksFor checks the DOL
    // hash, so this should never happen). Return without recolouring: the
    // model keeps its previous colours, and the game keeps running.
    state->pc = state->lr;
    return;
  }
  const std::uint32_t sp = state->gpr[1];
  moderngekko_mod_write(state, sp - 16, sp, 4);
  state->gpr[1] = sp - 16;
  state->pc = Setter + 4;
}

const ModernGekkoModPatch kBeachSpikersPatches[] = {
    RECOMP_INDIRECT_PATCH(0x80024ED8u, (PlayerColourSetter<0x80024ED8u, 0>)),
    RECOMP_INDIRECT_PATCH(0x80024F9Cu, (PlayerColourSetter<0x80024F9Cu, 1>)),
    RECOMP_INDIRECT_PATCH(0x80025060u, (PlayerColourSetter<0x80025060u, 2>)),
    RECOMP_INDIRECT_PATCH(0x80025128u, (PlayerColourSetter<0x80025128u, 3>)),
};

const ModernGekkoModDesc kBeachSpikersTweaks = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "GBSE8P",
    "builtin.beach_spikers",
    "1.0.0",
    "Beach Spikers gameplay options",
    nullptr,
    0u,
    kBeachSpikersPatches,
    static_cast<std::uint32_t>(sizeof(kBeachSpikersPatches) / sizeof(kBeachSpikersPatches[0])),
    nullptr,
    0u,
    nullptr,
    0u,
    nullptr,
    0u,
    nullptr,
    0u,
    nullptr,
    0u,
    nullptr,
    nullptr,
};
}  // namespace

void SetNeutralBallMarker(bool enabled)
{
  s_neutral_ball_marker.store(enabled, std::memory_order_relaxed);
}

bool NeutralBallMarker()
{
  return s_neutral_ball_marker.load(std::memory_order_relaxed);
}

bool SupportsNeutralBallMarker(const std::string& disc_id, const std::string& dol_sha256)
{
  return disc_id == "GBSE8P" && dol_sha256 == kBeachSpikersUsaDolSha256;
}

const ModernGekkoModDesc* BuiltinTweaksFor(const std::string& disc_id,
                                           const std::string& dol_sha256)
{
  if (SupportsNeutralBallMarker(disc_id, dol_sha256))
    return &kBeachSpikersTweaks;
  return nullptr;
}
}  // namespace moderngekko::tweaks
