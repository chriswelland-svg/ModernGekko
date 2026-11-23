#include "jersey.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#else
#include <sys/wait.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include <xxhash.h>

#include "Common/CommonTypes.h"
#include "Common/Image.h"
#include "Common/StringUtil.h"
#include "VideoCommon/TextureDecoder.h"

// stb_image from Dolphin's tinygltf checkout, compiled privately into this
// file (static functions) so it cannot clash with another copy in the link.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

namespace moderngekko::frontend::jersey {
namespace {
constexpr Team kTeams[] = {
    {"AUS", "Australia"}, {"BRA", "Brazil"},   {"CAN", "Canada"},
    {"CHN", "China"},     {"CUB", "Cuba"},     {"ENG", "England"},
    {"FRA", "France"},    {"GER", "Germany"},  {"ITA", "Italy"},
    {"JAM", "Jamaica"},   {"JPN", "Japan"},    {"MEX", "Mexico"},
    {"NED", "Netherlands"}, {"ESP", "Spain"},  {"THA", "Thailand"},
    {"USA", "USA"},
};

// The uniform: 512x512 CMPR (GX format 14), one level of data. The game
// samples it with mipmapping on, so Dolphin's name carries the "_m".
constexpr u32 kUniformSize = 512;
constexpr u32 kCmpr = 14;
constexpr std::size_t kUniformBytes = kUniformSize * kUniformSize / 2;
constexpr int kOut = 1024; // the replacement is drawn at twice the size

// Where a picture goes, in texture coordinates (0..1). The uniform wraps
// round the body: u = 0 is the middle of the back, u = 0.5 the middle of the
// front; rows 0.25-0.5 are the top. Found with a numbered test grid on the
// USA and Germany uniforms (re/jersey in the workspace). STRETCH is how much
// wider one unit of texture width looks on the body there than one unit of
// height, so a square picture stays square.
struct Spot {
  double cx, cy, max_w, max_h, stretch;
};
constexpr Spot kFront{0.50, 0.34, 0.20, 0.15, 1.7};
constexpr Spot kBack{0.00, 0.44, 0.24, 0.10, 1.1};

u32 Be32(const u8 *p) {
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}
u32 Be16(const u8 *p) { return (u32(p[0]) << 8) | u32(p[1]); }

std::vector<u8> ReadFile(const fs::path &path, std::size_t limit) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size <= 0 || static_cast<std::uint64_t>(size) > limit)
    return {};
  file.seekg(0, std::ios::beg);
  std::vector<u8> data(static_cast<std::size_t>(size));
  if (!file.read(reinterpret_cast<char *>(data.data()), size))
    return {};
  return data;
}

// Sega's TPL (as in Super Monkey Ball): u32 count, then 16-byte entries
// {u32 format, u32 offset, u16 width, u16 height, u16 levels, u16 0x1234},
// all big-endian.
bool ReadUniform(const fs::path &tpl, std::vector<u8> *data) {
  const std::vector<u8> buf = ReadFile(tpl, 8u << 20);
  if (buf.size() < 4)
    return false;
  const u32 count = Be32(buf.data());
  if (count == 0 || count > 256 || 4 + std::size_t(count) * 16 > buf.size())
    return false;
  for (u32 i = 0; i < count; ++i) {
    const u8 *e = buf.data() + 4 + std::size_t(i) * 16;
    const u32 format = Be32(e), offset = Be32(e + 4);
    const u32 width = Be16(e + 8), height = Be16(e + 10);
    if (format == kCmpr && width == kUniformSize && height == kUniformSize &&
        offset != 0 && std::size_t(offset) + kUniformBytes <= buf.size()) {
      data->assign(buf.begin() + offset, buf.begin() + offset + kUniformBytes);
      return true;
    }
  }
  return false;
}

// Dolphin's texture name: XXH64 of the base level (TextureInfo::
// CalculateTextureName), so it matches what the loader looks up.
std::string TextureName(const std::vector<u8> &data) {
  char name[64];
  std::snprintf(name, sizeof(name), "tex1_512x512_m_%016llx_14",
                static_cast<unsigned long long>(XXH64(data.data(), data.size(), 0)));
  return name;
}

struct TeamUniform {
  std::vector<u8> data;            // first file's texture data
  std::vector<std::string> names;  // every distinct texture of the team
};

// Both players' day and evening files. They share one uniform texture in
// every team we know of, but a texture that differs gets the picture too.
bool LoadTeam(const fs::path &game_root, std::string_view code, TeamUniform *out) {
  const fs::path chara = game_root / "files" / "chara";
  for (const char *player : {"1", "2"}) {
    for (const char *variant : {"DAY", "EVN"}) {
      const fs::path tpl = chara / ("CHA_" + std::string(code) + player + "_BODY_" +
                                    variant + ".tpl");
      std::vector<u8> data;
      if (!ReadUniform(tpl, &data))
        continue;
      std::string name = TextureName(data);
      if (std::find(out->names.begin(), out->names.end(), name) == out->names.end())
        out->names.push_back(std::move(name));
      if (out->data.empty())
        out->data = std::move(data);
    }
  }
  return !out->data.empty();
}

// 512 -> 1024, bilinear; x wraps (the uniform goes round the body), y clamps.
std::vector<u8> Upscale(const std::vector<u8> &src) {
  constexpr int in = int(kUniformSize);
  std::vector<u8> out(std::size_t(kOut) * kOut * 4);
  for (int y = 0; y < kOut; ++y) {
    const double sy = (y + 0.5) * in / kOut - 0.5;
    const int y0 = int(std::floor(sy));
    const double fy = sy - y0;
    const int ya = std::clamp(y0, 0, in - 1), yb = std::clamp(y0 + 1, 0, in - 1);
    for (int x = 0; x < kOut; ++x) {
      const double sx = (x + 0.5) * in / kOut - 0.5;
      const int x0 = int(std::floor(sx));
      const double fx = sx - x0;
      const int xa = (x0 % in + in) % in, xb = ((x0 + 1) % in + in) % in;
      for (int c = 0; c < 4; ++c) {
        const double top = src[(std::size_t(ya) * in + xa) * 4 + c] * (1 - fx) +
                           src[(std::size_t(ya) * in + xb) * 4 + c] * fx;
        const double bottom = src[(std::size_t(yb) * in + xa) * 4 + c] * (1 - fx) +
                              src[(std::size_t(yb) * in + xb) * 4 + c] * fx;
        out[(std::size_t(y) * kOut + x) * 4 + c] =
            u8(std::clamp(std::lround(top * (1 - fy) + bottom * fy), 0L, 255L));
      }
    }
  }
  return out;
}

// The picture resized to PW x PH, as premultiplied RGBA in 0..1. Shrinking
// averages every source pixel under each target pixel (weighted by overlap);
// growing is bilinear.
std::vector<float> Resample(const u8 *img, int w, int h, int pw, int ph) {
  std::vector<float> out(std::size_t(pw) * ph * 4, 0.0f);
  const auto pixel = [&](int x, int y, float *rgba) {
    const u8 *p = img + (std::size_t(y) * w + x) * 4;
    const float a = p[3] / 255.0f;
    rgba[0] = p[0] / 255.0f * a;
    rgba[1] = p[1] / 255.0f * a;
    rgba[2] = p[2] / 255.0f * a;
    rgba[3] = a;
  };
  const bool shrink = w >= pw && h >= ph;
  for (int oy = 0; oy < ph; ++oy) {
    for (int ox = 0; ox < pw; ++ox) {
      float *o = &out[(std::size_t(oy) * pw + ox) * 4];
      if (shrink) {
        const double x0 = double(ox) * w / pw, x1 = double(ox + 1) * w / pw;
        const double y0 = double(oy) * h / ph, y1 = double(oy + 1) * h / ph;
        double total = 0;
        double acc[4] = {0, 0, 0, 0};
        for (int iy = int(y0); iy < std::min(h, int(std::ceil(y1))); ++iy) {
          const double wy = std::min(y1, iy + 1.0) - std::max(y0, double(iy));
          for (int ix = int(x0); ix < std::min(w, int(std::ceil(x1))); ++ix) {
            const double wx = std::min(x1, ix + 1.0) - std::max(x0, double(ix));
            float rgba[4];
            pixel(ix, iy, rgba);
            for (int c = 0; c < 4; ++c)
              acc[c] += rgba[c] * wx * wy;
            total += wx * wy;
          }
        }
        for (int c = 0; c < 4; ++c)
          o[c] = total > 0 ? float(acc[c] / total) : 0.0f;
      } else {
        const double sx = std::clamp((ox + 0.5) * w / pw - 0.5, 0.0, w - 1.0);
        const double sy = std::clamp((oy + 0.5) * h / ph - 0.5, 0.0, h - 1.0);
        const int xa = int(sx), ya = int(sy);
        const int xb = std::min(xa + 1, w - 1), yb = std::min(ya + 1, h - 1);
        const double fx = sx - xa, fy = sy - ya;
        float p00[4], p10[4], p01[4], p11[4];
        pixel(xa, ya, p00);
        pixel(xb, ya, p10);
        pixel(xa, yb, p01);
        pixel(xb, yb, p11);
        for (int c = 0; c < 4; ++c)
          o[c] = float((p00[c] * (1 - fx) + p10[c] * fx) * (1 - fy) +
                       (p01[c] * (1 - fx) + p11[c] * fx) * fy);
      }
    }
  }
  return out;
}

// Paint the picture at SPOT, keeping its proportions on the body and its
// transparency (a logo with a transparent background keeps the uniform around
// it).
void Stamp(std::vector<u8> &dst, const u8 *img, int w, int h, const Spot &spot) {
  const double aspect = double(w) / h;
  const double hn = std::min(spot.max_h, spot.max_w * spot.stretch / aspect);
  const double wn = hn * aspect / spot.stretch;
  const int pw = std::max(1, int(std::lround(wn * kOut)));
  const int ph = std::max(1, int(std::lround(hn * kOut)));
  const std::vector<float> patch = Resample(img, w, h, pw, ph);
  const int x0 = int(std::lround(spot.cx * kOut - pw / 2.0));
  const int y0 = int(std::lround(spot.cy * kOut - ph / 2.0));
  for (int py = 0; py < ph; ++py) {
    const int y = y0 + py;
    if (y < 0 || y >= kOut)
      continue;
    for (int px = 0; px < pw; ++px) {
      const int x = ((x0 + px) % kOut + kOut) % kOut; // the back wraps at u = 0
      const float *s = &patch[(std::size_t(py) * pw + px) * 4];
      u8 *d = &dst[(std::size_t(y) * kOut + x) * 4];
      const float keep = 1.0f - s[3];
      for (int c = 0; c < 3; ++c)
        d[c] = u8(std::clamp(std::lround(s[c] * 255.0f + d[c] * keep), 0L, 255L));
      d[3] = u8(std::clamp(std::lround(s[3] * 255.0f + d[3] * keep), 0L, 255L));
    }
  }
}

std::string TeamLabel(std::string_view code) {
  const Team *team = FindTeam(code);
  return team ? team->name : std::string(code);
}

#if !defined(_WIN32)
// Run a dialog helper; returns its exit code (-1 when it could not start) and
// its first line of output.
int RunHelper(const std::string &command, std::string *line) {
  FILE *pipe = popen(command.c_str(), "r");
  if (!pipe)
    return -1;
  std::string output;
  std::array<char, 4096> chunk{};
  while (std::fgets(chunk.data(), int(chunk.size()), pipe))
    output += chunk.data();
  const int status = pclose(pipe);
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
    output.pop_back();
  if (const auto newline = output.find('\n'); newline != std::string::npos)
    output.resize(newline);
  *line = output;
  if (status == -1 || !WIFEXITED(status))
    return -1;
  return WEXITSTATUS(status);
}
#endif
} // namespace

std::span<const Team> Teams() { return kTeams; }

const Team *FindTeam(std::string_view code) {
  for (const Team &team : kTeams) {
    if (code == team.code)
      return &team;
  }
  return nullptr;
}

bool Supported(const fs::path &game_root) {
  std::error_code ec;
  return fs::is_regular_file(game_root / "files" / "chara" / "CHA_USA1_BODY_DAY.tpl", ec);
}

fs::path OutputDirectory(const fs::path &texture_directory) {
  return texture_directory / "moderngekko-jerseys";
}

Result Build(const fs::path &game_root, const fs::path &texture_directory,
             std::string_view code, const fs::path &image, Placement placement) {
  const std::string label = TeamLabel(code);
  if (!FindTeam(code))
    return {false, "unknown team " + std::string(code)};
  TeamUniform uniform;
  if (!LoadTeam(game_root, code, &uniform))
    return {false, "can't read the " + label + " uniform from the game files"};

  const std::vector<u8> file = ReadFile(image, 64u << 20);
  if (file.empty())
    return {false, "can't read " + PathToString(image)};
  int w = 0, h = 0, channels = 0;
  stbi_uc *pixels = stbi_load_from_memory(file.data(), int(file.size()), &w, &h,
                                          &channels, 4);
  if (!pixels)
    return {false, PathToString(image.filename()) +
                       " is not a PNG, JPEG, BMP or GIF image (" +
                       stbi_failure_reason() + ")"};
  if (w < 1 || h < 1 || w > 16384 || h > 16384) {
    stbi_image_free(pixels);
    return {false, "the image is too large (max 16384 x 16384)"};
  }

  std::vector<u8> base(std::size_t(kUniformSize) * kUniformSize * 4);
  TexDecoder_Decode(base.data(), uniform.data.data(), int(kUniformSize),
                    int(kUniformSize), TextureFormat::CMPR, nullptr, TLUTFormat::IA8);
  std::vector<u8> out = Upscale(base);
  if (placement.front)
    Stamp(out, pixels, w, h, kFront);
  if (placement.back)
    Stamp(out, pixels, w, h, kBack);
  stbi_image_free(pixels);

  const fs::path directory = OutputDirectory(texture_directory);
  std::error_code ec;
  fs::create_directories(directory, ec);
  if (ec)
    return {false, "can't create " + PathToString(directory) + ": " + ec.message()};
  for (const std::string &name : uniform.names) {
    // Written under a name the loader ignores, then renamed, so the texture
    // folder never holds a half-written PNG.
    const fs::path part = directory / ("." + name + ".part");
    const fs::path final_path = directory / (name + ".png");
    if (!Common::SavePNG(PathToString(part), out.data(), Common::ImageByteFormat::RGBA,
                         kOut, kOut, kOut * 4, 6))
      return {false, "can't write " + PathToString(final_path)};
    fs::rename(part, final_path, ec);
    if (ec) {
      fs::remove(part, ec);
      return {false, "can't write " + PathToString(final_path)};
    }
  }
  char size[48];
  std::snprintf(size, sizeof(size), " (%d x %d)", w, h);
  return {true, label + " jersey: " + PathToString(image.filename()) + size, uniform.names};
}

Result Remove(const fs::path &game_root, const fs::path &texture_directory,
              std::string_view code) {
  TeamUniform uniform;
  if (!LoadTeam(game_root, code, &uniform))
    return {false, "can't read the " + TeamLabel(code) + " uniform from the game files"};
  std::error_code ec;
  for (const std::string &name : uniform.names)
    fs::remove(OutputDirectory(texture_directory) / (name + ".png"), ec);
  return {true, TeamLabel(code) + " jersey back to the original", uniform.names};
}

bool Exists(const fs::path &game_root, const fs::path &texture_directory,
            std::string_view code) {
  TeamUniform uniform;
  if (!LoadTeam(game_root, code, &uniform))
    return false;
  std::error_code ec;
  for (const std::string &name : uniform.names) {
    if (!fs::is_regular_file(OutputDirectory(texture_directory) / (name + ".png"), ec))
      return false;
  }
  return true;
}

fs::path PickImage(std::string *error) {
#if defined(_WIN32)
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  std::array<wchar_t, 32768> file{};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFilter = L"Images (PNG, JPEG, BMP, GIF)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0"
                    L"All files\0*.*\0";
  ofn.lpstrFile = file.data();
  ofn.nMaxFile = DWORD(file.size());
  ofn.lpstrTitle = L"Choose a jersey image";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  fs::path chosen;
  if (GetOpenFileNameW(&ofn))
    chosen = fs::path(file.data());
  else if (const DWORD code = CommDlgExtendedError(); code != 0)
    *error = "the file dialog failed (error " + std::to_string(code) + ")";
  if (SUCCEEDED(com))
    CoUninitialize();
  return chosen;
#else
  std::string line;
#if defined(__APPLE__)
  const int code = RunHelper(
      "osascript -e 'POSIX path of (choose file with prompt \"Choose a jersey "
      "image\" of type {\"public.image\"})' 2>/dev/null",
      &line);
  if (code == 0 && !line.empty())
    return fs::path(line);
  if (code == 1)
    return {}; // cancelled
  *error = "no file dialog available; type the image's path instead";
  return {};
#else
  // The AppImage puts its own libraries first on LD_LIBRARY_PATH; the desktop's
  // dialog tools need the system's.
  int code = RunHelper(
      "env -u LD_LIBRARY_PATH zenity --file-selection --title='Choose a jersey "
      "image' --file-filter='Images | *.png *.PNG *.jpg *.JPG *.jpeg *.JPEG *.bmp "
      "*.BMP *.gif *.GIF' --file-filter='All files | *' 2>/dev/null",
      &line);
  if (code == 0 && !line.empty())
    return fs::path(line);
  if (code == 1)
    return {}; // cancelled
  code = RunHelper("env -u LD_LIBRARY_PATH kdialog --title 'Choose a jersey image' "
                   "--getopenfilename \"$HOME\" '*.png *.jpg *.jpeg *.bmp *.gif|Images' "
                   "2>/dev/null",
                   &line);
  if (code == 0 && !line.empty())
    return fs::path(line);
  if (code == 1)
    return {};
  *error = "no file dialog found (install zenity or kdialog), or type the "
           "image's path instead";
  return {};
#endif
#endif
}
} // namespace moderngekko::frontend::jersey
