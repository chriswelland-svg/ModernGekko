#pragma once

#include "frontend_config.hpp"
#include "moderngekko/runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace moderngekko::frontend {
enum class NetplayRole {
  Host,
  Join,
};

enum class NetplayExitCode {
  Failed = 1,
  InvalidConfiguration = 2,
  HostUnavailable = 10,
  VersionMismatch = 11,
  CompatibilityMismatch = 12,
  RoomFull = 13,
  GameRunning = 14,
  ServerFull = 15,
  NicknameRejected = 16,
};

struct NetplayOptions {
  NetplayRole role = NetplayRole::Join;
  std::string address = "127.0.0.1";
  std::uint16_t port = 2626;
  std::string nickname = "Player";
  std::string buffer = "auto";
  std::vector<std::string> controllers;
};

// Gameplay options the host sends with each game (bit flags). Every machine
// runs the host's set: they change which game code runs.
inline constexpr std::uint32_t kNetplayNeutralBallMarker = 1u << 0;

struct NetplaySessionHooks {
  // The game is created and not running yet: apply this machine's settings.
  // GAMEPLAY_OPTIONS are the host's; BANNER names the teams.
  std::function<void(Runtime &runtime, ConfigResult &frontend_config,
                     std::uint32_t gameplay_options,
                     const std::string &banner)>
      before_run;
  // Run() returned.
  std::function<void()> after_run;
};

// Address text a player typed: "host", "host:port" or "[v6]:port". A port in
// the text wins over PORT.
struct NetplayAddress {
  std::string host;
  std::uint16_t port = 2626;
  bool ok = false;
};
inline NetplayAddress ParseNetplayAddress(std::string_view text, std::uint16_t port) {
  NetplayAddress result;
  result.port = port;
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                           text.back() == '\r' || text.back() == '\n'))
    text.remove_suffix(1);
  if (text.empty())
    return result;
  std::string_view host = text;
  std::string_view port_text;
  if (text.front() == '[') {
    const std::size_t close = text.find(']');
    if (close == std::string_view::npos)
      return result;
    host = text.substr(1, close - 1);
    if (close + 1 < text.size()) {
      if (text[close + 1] != ':')
        return result;
      port_text = text.substr(close + 2);
    }
  } else if (std::ranges::count(text, ':') == 1) {
    const std::size_t colon = text.find(':');
    host = text.substr(0, colon);
    port_text = text.substr(colon + 1);
  }
  if (host.empty() || host.find_first_of(" \t/\\") != std::string_view::npos)
    return result;
  if (!port_text.empty()) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), value);
    if (error != std::errc{} || end != port_text.data() + port_text.size() ||
        value == 0 || value > 65535)
      return result;
    result.port = static_cast<std::uint16_t>(value);
  }
  result.host = std::string(host);
  result.ok = true;
  return result;
}


int RunNetplayLobby(RuntimeConfig runtime_config, ConfigResult frontend_config,
                    NetplayOptions options, NetplaySessionHooks hooks = {});
} // namespace moderngekko::frontend
