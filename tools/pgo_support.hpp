#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moderngekko::pgo
{
namespace fs = std::filesystem;

enum class PgoMode
{
  Off,
  Generate,
  Use,
};

std::string_view PgoModeName(PgoMode mode);

struct PgoBuildOptions
{
  PgoMode mode = PgoMode::Off;
  fs::path merged_profile;
  std::string merged_profile_sha256;
  bool reject_stale_profile = true;
};

struct Workspace
{
  fs::path root;
  fs::path raw;
  fs::path generate_modules;
  fs::path merged_profile;
  fs::path manifest;
};

Workspace DeriveWorkspace(const fs::path& profile_dir, const fs::path& output,
                          std::string_view disc_id);

inline constexpr std::size_t WINDOWS_PATH_LIMIT = 260;

// The per-build cache directory under <output>/<disc>/. Linux and macOS keep
// the descriptive "<dol sha256>-<16 hex identity>" name. Windows uses a 16-hex
// digest of both: the 81-character name alone pushed CMake's first compiler
// check (module-build/CMakeFiles/CMakeScratch/TryCompile-*/...) past MAX_PATH
// under C:\Users\<name>\AppData\Local for every user, and ninja then could not
// create its response file ("WriteFile(...rsp): Unable to create file").
inline constexpr std::size_t FULL_CACHE_KEY_LENGTH = 64 + 1 + 16;
inline constexpr std::size_t SHORT_CACHE_KEY_LENGTH = 16;
#if defined(_WIN32)
inline constexpr std::size_t CACHE_KEY_LENGTH = SHORT_CACHE_KEY_LENGTH;
#else
inline constexpr std::size_t CACHE_KEY_LENGTH = FULL_CACHE_KEY_LENGTH;
#endif

// The longest file the module build creates under cache_root: a chunk's ninja
// response file, or the response file of CMake's compiler check, whichever is
// longer.
std::size_t LongestModuleObjectPathLength(const fs::path& cache_root, std::string_view disc_id,
                                          std::size_t cache_key_length = CACHE_KEY_LENGTH);

std::string PathText(const fs::path& path);

std::string ClangPathText(const fs::path& path);

struct ProfdataSummary
{
  std::uint64_t total_functions = 0;
  std::uint64_t maximum_function_count = 0;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

ProfdataSummary ParseProfdataSummary(std::string_view text);

std::optional<int> ParseLlvmMajorVersion(std::string_view text);

std::vector<fs::path> DiscoverRawProfiles(const fs::path& raw_directory);

struct LlvmProfdataSources
{
  fs::path explicit_path;
  fs::path environment_path;
  fs::path clang_path;
  fs::path print_prog_name;
  fs::path xcrun_path;
};

std::vector<fs::path> LlvmProfdataCandidates(const LlvmProfdataSources& sources);

std::string PgoCacheIdentity(const PgoBuildOptions& options);

struct ManifestFacts
{
  std::string backend;
  std::string clang_version;
  std::string llvm_profdata_version;
  std::size_t raw_profile_count = 0;
  std::uint64_t total_functions = 0;
  std::uint64_t maximum_function_count = 0;
};

std::string FormatPgoManifest(const PgoBuildOptions& options, const ManifestFacts& facts);
}  // namespace moderngekko::pgo
