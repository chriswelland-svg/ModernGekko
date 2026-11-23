#pragma once

// Team jerseys (Beach Spikers): put a picture of the player's choosing on a
// national team's uniform.
//
// Nothing of the game ships with this. Each team's uniform is one 512x512
// CMPR texture inside the player's own disc files
// (files/chara/CHA_<team>1_BODY_DAY.tpl; both players, day and evening share
// it). The builder decodes that texture, draws the picture on the front and
// the back of the top, and writes the result as an HD texture-pack file named
// after the texture's Dolphin hash, which is computed from the same file. The
// HD texture loader then swaps it in.

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moderngekko::frontend::jersey {
struct Team {
  const char *code; // the game's own file code, e.g. "USA"
  const char *name;
};

// The 16 national teams, in alphabetical order of their names.
std::span<const Team> Teams();
const Team *FindTeam(std::string_view code);

struct Placement {
  bool front = true; // chest
  bool back = true;  // back of the top, the side the camera sees in play
};

// True when the game files hold the uniform textures (Beach Spikers, any
// region).
bool Supported(const std::filesystem::path &game_root);

// Where the generated files go: a subfolder of the game's HD texture folder,
// so the loader finds them and they never mix with a pack's own files.
std::filesystem::path OutputDirectory(const std::filesystem::path &texture_directory);

struct Result {
  bool ok = false;
  std::string message;               // for the status line
  std::vector<std::string> textures; // Dolphin texture ids written or deleted
};

// Draw IMAGE (PNG, JPEG, BMP or GIF) on TEAM's uniform and write the texture
// file. Takes a fraction of a second: run it off the video thread.
Result Build(const std::filesystem::path &game_root,
             const std::filesystem::path &texture_directory,
             std::string_view team, const std::filesystem::path &image,
             Placement placement);

// Delete TEAM's generated texture (ok when there was none).
Result Remove(const std::filesystem::path &game_root,
              const std::filesystem::path &texture_directory,
              std::string_view team);

// Whether TEAM's generated texture is on disk.
bool Exists(const std::filesystem::path &game_root,
            const std::filesystem::path &texture_directory,
            std::string_view team);

// The system's "open file" dialog, filtered to images. Blocks until the player
// picks or cancels, so call it on a worker thread. Returns an empty path on
// cancel; sets *error (and returns empty) when no dialog could be shown.
std::filesystem::path PickImage(std::string *error);
} // namespace moderngekko::frontend::jersey
