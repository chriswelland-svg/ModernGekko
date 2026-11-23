#include "netplay_session.hpp"

#include "netplay_compatibility.hpp"

#include "Common/Config/Config.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/NetPlay/NetPlayServer.h"
#include "Core/PowerPC/PowerPC.h"
#include "UICommon/GameFile.h"
#include "UICommon/UICommon.h"
#include "runtime/dolphin_runtime_internal.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace moderngekko::frontend {

namespace {
struct ControllerOption {
  std::string label;
  std::string device;
};

std::vector<ControllerOption> EnumerateControllers() {
  std::vector<ControllerOption> result;
  std::unordered_map<std::string, int> device_ids;
  int count = 0;
  SDL_JoystickID *joystick_ids = SDL_GetJoysticks(&count);
  for (int i = 0; i < count; ++i) {
    const SDL_JoystickID joystick_id = joystick_ids[i];
    if (!SDL_IsGamepad(joystick_id))
      continue;
    const char *name_value = SDL_GetGamepadNameForID(joystick_id);
    const std::string name =
        name_value && *name_value ? name_value : "Unknown Controller";
    const int device_id = device_ids[name]++;
    result.push_back({device_id == 0
                          ? name
                          : name + " (" + std::to_string(device_id + 1) + ")",
                      "SDL/" + std::to_string(device_id) + "/" + name});
  }
  SDL_free(joystick_ids);
  return result;
}

// "SDL/0/Xbox 360 Controller" -> "Xbox 360 Controller"; "Keyboard|..." ->
// "Keyboard".
std::string ControllerLabel(const std::string &device) {
  if (device.starts_with("Keyboard"))
    return "Keyboard";
  const std::size_t second = device.find('/', device.find('/') + 1);
  if (second != std::string::npos && device.find('/') != std::string::npos)
    return device.substr(second + 1);
  return device;
}

// Test hooks, from MODERNGEKKO_NETPLAY_TEST (comma-separated):
//   start=N    host: start once N controller ports are filled and all ready
//   shuffle    host: shuffle the teams once before that start
//   shot=PFX   save the lobby to PFX-<n>.bmp every two seconds (max 40)
//   leave=N    leave the lobby after N games
struct TestHooks {
  int start_at = 0;
  bool shuffle = false;
  std::string shot_prefix;
  int leave_after = 0;
};

TestHooks ReadTestHooks() {
  TestHooks hooks;
  const char *value = std::getenv("MODERNGEKKO_NETPLAY_TEST");
  if (!value)
    return hooks;
  std::string_view text = value;
  while (!text.empty()) {
    const std::size_t comma = text.find(',');
    const std::string_view token = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view{}
                                           : text.substr(comma + 1);
    if (token.starts_with("start="))
      hooks.start_at = std::atoi(std::string(token.substr(6)).c_str());
    else if (token == "shuffle")
      hooks.shuffle = true;
    else if (token.starts_with("shot="))
      hooks.shot_prefix = std::string(token.substr(5));
    else if (token.starts_with("leave="))
      hooks.leave_after = std::atoi(std::string(token.substr(6)).c_str());
  }
  return hooks;
}

class SessionUI final : public NetPlay::NetPlayUI {
public:
  explicit SessionUI(std::shared_ptr<UICommon::GameFile> game)
      : m_game(std::move(game)) {}

  void SetClient(NetPlay::NetPlayClient *client) {
    std::lock_guard lock(m_mutex);
    m_client = client;
  }

  void SetHosting(bool hosting) {
    std::lock_guard lock(m_mutex);
    m_hosting = hosting;
  }

  void SetRuntime(Runtime *runtime) {
    std::lock_guard lock(m_mutex);
    m_runtime = runtime;
  }

  bool TakeStartRequest() { return m_start_requested.exchange(false); }

  std::unique_ptr<BootSessionData> TakeBootData() {
    std::lock_guard lock(m_mutex);
    return std::move(m_boot_data);
  }

  std::string Error() {
    std::lock_guard lock(m_mutex);
    return m_error;
  }

  void ClearError() {
    std::lock_guard lock(m_mutex);
    m_error.clear();
  }

  // The lobby's event list, newest last.
  std::vector<std::string> Events() {
    std::lock_guard lock(m_mutex);
    return {m_events.begin(), m_events.end()};
  }

  void AddEvent(std::string text) {
    std::lock_guard lock(m_mutex);
    AddEventLocked(std::move(text));
  }

  // "Sending the memory card..." while a start is being prepared.
  std::string Progress() {
    std::lock_guard lock(m_mutex);
    if (m_progress_title.empty())
      return {};
    if (m_progress_size == 0)
      return m_progress_title;
    const int percent = static_cast<int>(
        std::min<u64>(100, m_progress * 100 / std::max<u64>(1, m_progress_size)));
    return m_progress_title + " " + std::to_string(percent) + "%";
  }

  u32 BufferSize() const { return m_buffer_size.load(); }

  bool ConnectionLost() const { return m_connection_lost.load(); }

  void BootGame(const std::string &,
                std::unique_ptr<BootSessionData> boot_session_data) override {
    std::lock_guard lock(m_mutex);
    m_boot_data = std::move(boot_session_data);
  }

  void StopGame() override {
    std::lock_guard lock(m_mutex);
    if (m_runtime)
      m_runtime->RequestStop();
  }

  bool IsHosting() const override { return m_hosting; }

  void Update() override {}

  void AppendChat(const std::string &msg) override {
    // Dolphin's own "Quality of Service (QoS) ..." notes mean nothing to a
    // player: log them, keep them out of the lobby.
    if (msg.find("Quality of Service") != std::string::npos) {
      std::cerr << "netplay: " << msg << '\n';
      return;
    }
    AddEvent(msg);
  }

  void OnMsgChangeGame(const NetPlay::SyncIdentifier &,
                       const std::string &) override {}

  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig &) override {}

  void OnMsgStartGame() override { m_start_requested = true; }

  void OnMsgStopGame() override {
    AddEvent("The game was stopped: back in the lobby");
    StopGame();
  }

  void OnMsgPowerButton() override { StopGame(); }

  void OnPlayerConnect(const std::string &player) override {
    AddEvent(player + " joined");
  }

  void OnPlayerDisconnect(const std::string &player) override {
    AddEvent(player + " left");
  }

  void OnPadBufferChanged(u32 buffer) override { m_buffer_size = buffer; }

  void OnHostInputAuthorityChanged(bool) override {}

  void OnDesync(u32 frame, const std::string &player) override {
    {
      std::lock_guard lock(m_mutex);
      m_error = "The games drifted apart (desync at frame " +
                std::to_string(frame) + (player.empty() ? "" : ", " + player) +
                "), so it was stopped. Press Start to play again; if it "
                "keeps happening, check that everyone uses the same download "
                "and the same disc.";
      AddEventLocked("Desync: game stopped");
    }
    StopGame();
  }

  void OnConnectionLost() override {
    m_connection_lost = true;
    {
      std::lock_guard lock(m_mutex);
      m_error = m_hosting ? "The lobby stopped unexpectedly."
                          : "The connection to the host was lost. The host may "
                            "have closed the lobby, or the network dropped.";
      AddEventLocked("Connection lost");
    }
    StopGame();
  }

  void OnConnectionError(const std::string &message) override {
    std::lock_guard lock(m_mutex);
    m_error = message;
  }

  void OnTraversalError(Common::TraversalClient::FailureReason) override {
    OnConnectionError("Direct connection failed");
  }

  void OnTraversalStateChanged(Common::TraversalClient::State) override {}

  void OnGameStartAborted() override {
    std::lock_guard lock(m_mutex);
    m_error = "The start was cancelled (a player left or could not receive "
              "the game data). Press Start again.";
  }

  void OnGolferChanged(bool, const std::string &) override {}
  void OnTtlDetermined(u8) override {}
  bool IsRecording() override { return false; }

  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier &sync_identifier,
               NetPlay::SyncIdentifierComparison *found) override {
    const auto comparison = m_game->CompareSyncIdentifier(sync_identifier);
    if (found)
      *found = comparison;
    return m_game;
  }

  std::string FindGBARomPath(const std::array<u8, 20> &, std::string_view,
                             int) override {
    return {};
  }

  void ShowGameDigestDialog(const std::string &) override {}
  void SetGameDigestProgress(int, int) override {}
  void SetGameDigestResult(int, const std::string &) override {}
  void AbortGameDigest() override {}
  void OnIndexAdded(bool, std::string) override {}
  void OnIndexRefreshFailed(std::string) override {}

  void ShowChunkedProgressDialog(const std::string &title, u64 data_size,
                                 std::span<const int>) override {
    std::lock_guard lock(m_mutex);
    m_progress_title = title;
    m_progress_size = data_size;
    m_progress = 0;
  }

  void HideChunkedProgressDialog() override {
    std::lock_guard lock(m_mutex);
    m_progress_title.clear();
    m_progress_size = 0;
    m_progress = 0;
  }

  void SetChunkedProgress(int, u64 progress) override {
    std::lock_guard lock(m_mutex);
    m_progress = std::max(m_progress, progress);
  }

  void SetHostWiiSyncData(std::vector<u64> titles,
                          std::string redirect_folder) override {
    std::lock_guard lock(m_mutex);
    if (m_client)
      m_client->SetWiiSyncData(nullptr, std::move(titles),
                               std::move(redirect_folder));
  }

private:
  void AddEventLocked(std::string text) {
    std::cerr << "netplay: " << text << '\n';
    m_events.push_back(std::move(text));
    while (m_events.size() > 4)
      m_events.pop_front();
  }

  std::shared_ptr<UICommon::GameFile> m_game;
  mutable std::mutex m_mutex;
  NetPlay::NetPlayClient *m_client = nullptr;
  Runtime *m_runtime = nullptr;
  bool m_hosting = false;
  std::atomic<bool> m_start_requested{false};
  std::atomic<bool> m_connection_lost{false};
  std::atomic<u32> m_buffer_size{5};
  std::unique_ptr<BootSessionData> m_boot_data;
  std::string m_error;
  std::deque<std::string> m_events;
  std::string m_progress_title;
  u64 m_progress_size = 0;
  u64 m_progress = 0;
};

class LobbyWindow {
public:
  bool Open(WindowSystem window_system, const std::string &title) {
#if defined(__linux__)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,
                window_system == WindowSystem::Wayland ? "wayland" : "x11");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
      return false;
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    m_window = SDL_CreateWindow(
        title.c_str(), static_cast<int>(980 * scale),
        static_cast<int>(800 * scale),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m_window)
      return false;
    m_renderer = SDL_CreateRenderer(m_window, "vulkan");
    if (!m_renderer)
      m_renderer = SDL_CreateRenderer(m_window, nullptr);
    if (!m_renderer)
      return false;
    SDL_SetRenderVSync(m_renderer, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(18, 14);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.FrameRounding = 5;
    style.ChildRounding = 6;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    // Larger than ImGui's default 13 px: this window is read from the couch.
    style.FontScaleMain = 1.3f;
    ImGui_ImplSDL3_InitForSDLRenderer(m_window, m_renderer);
    ImGui_ImplSDLRenderer3_Init(m_renderer);
    m_scale = scale;
    m_open = true;
    return true;
  }

  ~LobbyWindow() { Close(); }

  void Close() {
    if (!m_open)
      return;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(m_renderer);
    SDL_DestroyWindow(m_window);
    SDL_Quit();
    m_renderer = nullptr;
    m_window = nullptr;
    m_open = false;
  }

  bool Frame() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT ||
          event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        return false;
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    return true;
  }

  void Present(const std::string &shot_path) {
    ImGui::Render();
    const ImGuiIO &io = ImGui::GetIO();
    SDL_SetRenderScale(m_renderer, io.DisplayFramebufferScale.x,
                       io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(m_renderer, 18, 20, 28, 255);
    SDL_RenderClear(m_renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_renderer);
    if (!shot_path.empty()) {
      if (SDL_Surface *frame = SDL_RenderReadPixels(m_renderer, nullptr)) {
        SDL_SaveBMP(frame, shot_path.c_str());
        SDL_DestroySurface(frame);
      }
    }
    SDL_RenderPresent(m_renderer);
  }

  float Scale() const { return m_scale; }

private:
  SDL_Window *m_window = nullptr;
  SDL_Renderer *m_renderer = nullptr;
  float m_scale = 1.0f;
  bool m_open = false;
};

constexpr ImVec4 kGreen(0.45f, 0.85f, 0.45f, 1.0f);
constexpr ImVec4 kYellow(0.95f, 0.8f, 0.3f, 1.0f);
constexpr ImVec4 kRed(1.0f, 0.4f, 0.35f, 1.0f);
constexpr ImVec4 kLeftTeam(0.95f, 0.55f, 0.35f, 1.0f);
constexpr ImVec4 kRightTeam(0.4f, 0.7f, 1.0f, 1.0f);

struct Seat {
  NetPlay::PlayerId pid = 0;
  std::string name; // "" when nobody sits here
};

// Port -> player name ("Chris", "Chris (2nd pad)").
std::array<Seat, 4> Seats(const NetPlay::PadMappingArray &mapping,
                          const std::vector<NetPlay::Player> &players) {
  std::array<Seat, 4> seats{};
  std::unordered_map<NetPlay::PlayerId, int> seen;
  for (std::size_t i = 0; i < mapping.size(); ++i) {
    const NetPlay::PlayerId pid = mapping[i];
    if (pid == 0)
      continue;
    const auto player = std::ranges::find(players, pid, &NetPlay::Player::pid);
    std::string name =
        player != players.end() ? player->name : "Player " + std::to_string(pid);
    const int nth = ++seen[pid];
    if (nth > 1)
      name += " (pad " + std::to_string(nth) + ")";
    seats[i] = {pid, std::move(name)};
  }
  return seats;
}

std::string TeamText(const Seat &a, const Seat &b) {
  if (a.name.empty() && b.name.empty())
    return "computer";
  if (a.name.empty() || b.name.empty())
    return (a.name.empty() ? b.name : a.name) + " + computer";
  return a.name + " & " + b.name;
}

// The line every machine shows when the game starts.
std::string TeamBanner(const std::array<Seat, 4> &seats) {
  return "Online - Left team: " + TeamText(seats[0], seats[1]) +
         "   |   Right team: " + TeamText(seats[2], seats[3]);
}

std::string MappingText(const std::array<Seat, 4> &seats) {
  std::string text;
  for (std::size_t i = 0; i < seats.size(); ++i) {
    if (i)
      text += ' ';
    text += std::to_string(i + 1) + '=' +
            (seats[i].name.empty() ? std::string("-") : seats[i].name);
  }
  return text;
}

// Port 1 runs the menus, so a shuffle keeps it filled.
NetPlay::PadMappingArray Shuffled(NetPlay::PadMappingArray mapping,
                                  std::mt19937 &random) {
  const NetPlay::PadMappingArray before = mapping;
  for (int attempt = 0; attempt < 12; ++attempt) {
    std::ranges::shuffle(mapping, random);
    if (mapping[0] == 0)
      std::swap(mapping[0], mapping[1]);
    if (mapping[0] == 0)
      std::swap(mapping[0], mapping[mapping[2] != 0 ? 2 : 3]);
    if (mapping != before)
      break;
  }
  return mapping;
}

struct AddressRow {
  std::string address; // what friends type
  std::string where;   // "same home network", "Tailscale", ...
};

bool StartsWith(const std::string &text, std::string_view prefix) {
  return text.rfind(prefix, 0) == 0;
}

// Addresses worth giving to friends, best first. Interface names that are
// never reachable by them (Docker bridges, VM switches) are left out.
std::vector<AddressRow> InviteAddresses(NetPlay::NetPlayServer &server) {
  std::vector<AddressRow> rows;
  for (const std::string &name : server.GetInterfaceSet()) {
    if (StartsWith(name, "docker") || StartsWith(name, "br-") ||
        StartsWith(name, "veth") || StartsWith(name, "virbr") ||
        StartsWith(name, "vmnet") || StartsWith(name, "lo"))
      continue;
    std::string host = server.GetInterfaceHost(name);
    const std::size_t colon = host.rfind(':');
    if (colon != std::string::npos)
      host.resize(colon);
    if (host.empty() || host == "?" || StartsWith(host, "127.") ||
        StartsWith(host, "169.254."))
      continue;
    std::string where = "other network";
    int second = -1;
    if (std::sscanf(host.c_str(), "100.%d.", &second) == 1 && second >= 64 &&
        second <= 127)
      where = "Tailscale / VPN (friends in other homes)";
    else if (StartsWith(host, "192.168.") || StartsWith(host, "10."))
      where = "same home or Wi-Fi network";
    else if (std::sscanf(host.c_str(), "172.%d.", &second) == 1 &&
             second >= 16 && second <= 31)
      where = "same home or Wi-Fi network";
    if (server.GetPort() != 2626)
      host += ':' + std::to_string(server.GetPort());
    rows.push_back({std::move(host), std::move(where)});
  }
  std::ranges::sort(rows, [](const AddressRow &a, const AddressRow &b) {
    const auto rank = [](const AddressRow &row) {
      return row.where.starts_with("same") ? 0
             : row.where.starts_with("Tail") ? 1
                                             : 2;
    };
    return std::pair(rank(a), a.address) < std::pair(rank(b), b.address);
  });
  return rows;
}

void HelpMarker(const char *text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

struct LobbyState {
  std::string title;
  bool gamecube = true;
  TestHooks test;
  int games_played = 0;
  bool auto_ready_sent = false;
  std::mt19937 random{std::random_device{}()};
  std::string last_mapping;
  int shot_index = 0;
  std::chrono::steady_clock::time_point last_shot{};
  std::string copied;
  std::chrono::steady_clock::time_point copied_at{};
};

enum class LobbyResult {
  Boot,
  Leave,
};

LobbyResult RunLobbyWindow(RuntimeConfig &runtime_config,
                           ConfigResult &frontend_config,
                           NetplayOptions &options, SessionUI &ui,
                           NetPlay::NetPlayClient &client,
                           NetPlay::NetPlayServer *server, LobbyState &state,
                           std::string *banner) {
  LobbyWindow window;
  if (!window.Open(runtime_config.window_system, state.title))
    return LobbyResult::Leave;
  const float scale = window.Scale();
  state.auto_ready_sent = false;
  // Test hooks act once per lobby visit (so a second game can be tested).
  bool test_started = false;
  bool test_shuffled = false;

  std::vector<ControllerOption> available = EnumerateControllers();
  auto last_refresh = std::chrono::steady_clock::now();
  while (true) {
    if (!window.Frame())
      return LobbyResult::Leave;

    const auto now = std::chrono::steady_clock::now();
    if (now - last_refresh >= std::chrono::seconds(1)) {
      available = EnumerateControllers();
      last_refresh = now;
    }

    const std::vector<NetPlay::Player> players = client.GetPlayersSnapshot();
    const NetPlay::PadMappingArray mapping =
        state.gamecube ? client.GetPadMappingSnapshot()
                       : client.GetWiimoteMappingSnapshot();
    const std::array<Seat, 4> seats = Seats(mapping, players);
    const std::string mapping_text = MappingText(seats);
    if (mapping_text != state.last_mapping) {
      state.last_mapping = mapping_text;
      std::cerr << "netplay: ports " << mapping_text << '\n';
    }
    const bool lost = ui.ConnectionLost();
    const u8 assigned = client.GetAssignedControllerCount();
    if (assigned > 0 && options.controllers.size() > assigned) {
      options.controllers.resize(assigned);
      frontend_config.controllers = options.controllers;
      frontend_config.controller = options.controllers.front();
      std::string ignored;
      SaveConfig(runtime_config.user_directory, frontend_config, &ignored);
    }

    const auto local_player =
        std::ranges::find_if(players, [&](const NetPlay::Player &player) {
          return client.IsLocalPlayer(player.pid);
        });
    const bool have_local = local_player != players.end();
    const bool local_ready = have_local && local_player->ready;
    // Ready by default: the host only has to press Start. "Wait" holds it.
    if (!state.auto_ready_sent && have_local && !lost &&
        local_player->controller_count > 0 &&
        local_player->game_status ==
            NetPlay::SyncIdentifierComparison::SameGame) {
      client.SetReady(true);
      state.auto_ready_sent = true;
    }

    if (server && state.test.start_at > 0 && !test_started && !lost) {
      const auto filled = std::ranges::count_if(
          mapping, [](NetPlay::PlayerId pid) { return pid != 0; });
      if (filled >= state.test.start_at) {
        if (state.test.shuffle && !test_shuffled) {
          test_shuffled =
              server->ArrangeControllers(Shuffled(mapping, state.random));
          std::cerr << "netplay: test shuffle "
                    << (test_shuffled ? "sent" : "refused") << '\n';
        } else if (server->CanStart() && ui.Progress().empty() &&
                   server->RequestStartGame()) {
          std::cerr << "netplay: test start with ports " << mapping_text
                    << '\n';
          test_started = true;
        }
      }
    }

    if (ui.TakeStartRequest())
      client.StartGame(runtime_config.game_root.string() + "/sys/main.dol");
    if (auto boot_data = ui.TakeBootData()) {
      detail::SetBootSessionData(std::move(boot_data));
      *banner = TeamBanner(seats);
      std::cerr << "netplay: starting with ports " << mapping_text << '\n';
      return LobbyResult::Boot;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##lobby", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);

    // --- Header ------------------------------------------------------------
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
    ImGui::TextUnformatted(state.title.c_str());
    ImGui::PopFont();
    if (server)
      ImGui::TextColored(kGreen, "You are hosting. Your friends join you.");
    else
      ImGui::TextColored(kGreen, "Joined the host at %s:%u",
                         options.address.c_str(), options.port);
    ImGui::Separator();

    // --- Invite (host) -----------------------------------------------------
    if (server) {
      ImGui::TextUnformatted("1. Give your friends this address:");
      const std::vector<AddressRow> rows = InviteAddresses(*server);
      if (rows.empty())
        ImGui::TextColored(kYellow, "   No network found. Is this computer "
                                    "connected?");
      for (const AddressRow &row : rows) {
        ImGui::PushID(row.address.c_str());
        ImGui::Text("   ");
        ImGui::SameLine();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
        ImGui::TextColored(kYellow, "%s", row.address.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
          SDL_SetClipboardText(row.address.c_str());
          state.copied = row.address;
          state.copied_at = now;
        }
        ImGui::SameLine();
        if (state.copied == row.address &&
            now - state.copied_at < std::chrono::seconds(2))
          ImGui::TextColored(kGreen, "copied");
        else
          ImGui::TextDisabled("%s", row.where.c_str());
        ImGui::PopID();
      }
      ImGui::TextDisabled("   Friends in another home: everyone installs "
                          "Tailscale and uses its address,");
      ImGui::TextDisabled("   or forward UDP port %u on your router to this "
                          "computer. See the online play guide.",
                          server->GetPort());
      ImGui::Spacing();
    }

    // --- Teams -------------------------------------------------------------
    ImGui::TextUnformatted(server ? "2. Teams (when everyone is in):"
                                  : "Teams (the host sets these):");
    const float team_width =
        (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) /
        2.0f;
    std::optional<NetPlay::PadMappingArray> rearranged;
    for (int side = 0; side < 2; ++side) {
      if (side == 1)
        ImGui::SameLine();
      ImGui::BeginChild(side == 0 ? "##left" : "##right",
                        ImVec2(team_width,
                               ImGui::GetTextLineHeightWithSpacing() +
                                   ImGui::GetFrameHeightWithSpacing() * 2 +
                                   ImGui::GetStyle().WindowPadding.y * 2),
                        ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_NoScrollbar);
      ImGui::TextColored(side == 0 ? kLeftTeam : kRightTeam,
                         side == 0 ? "LEFT TEAM" : "RIGHT TEAM");
      for (int row = 0; row < 2; ++row) {
        const int port = side * 2 + row;
        ImGui::PushID(port);
        ImGui::Text("Port %d", port + 1);
        ImGui::SameLine(80 * scale);
        const Seat &seat = seats[static_cast<std::size_t>(port)];
        const std::string preview =
            seat.name.empty() ? "(open - computer plays)" : seat.name;
        if (server) {
          ImGui::SetNextItemWidth(-FLT_MIN);
          if (ImGui::BeginCombo("##seat", preview.c_str())) {
            // Picking who sits here swaps them with this seat.
            for (int other = 0; other < 4; ++other) {
              const Seat &candidate = seats[static_cast<std::size_t>(other)];
              if (candidate.name.empty() && other != port &&
                  std::ranges::any_of(seats | std::views::take(other),
                                      [](const Seat &s) {
                                        return s.name.empty();
                                      }))
                continue; // list one "open" entry, not each open port
              const std::string label =
                  candidate.name.empty() ? "(open - computer plays)"
                                         : candidate.name;
              if (ImGui::Selectable((label + "##" + std::to_string(other)).c_str(),
                                    other == port) &&
                  other != port) {
                NetPlay::PadMappingArray next = mapping;
                std::swap(next[static_cast<std::size_t>(port)],
                          next[static_cast<std::size_t>(other)]);
                rearranged = next;
              }
            }
            ImGui::EndCombo();
          }
        } else if (seat.name.empty()) {
          ImGui::TextDisabled("%s", preview.c_str());
        } else {
          ImGui::TextUnformatted(preview.c_str());
        }
        ImGui::PopID();
      }
      ImGui::EndChild();
    }
    if (server) {
      ImGui::BeginDisabled(lost);
      if (ImGui::Button("Shuffle teams", ImVec2(170 * scale, 0)))
        rearranged = Shuffled(mapping, state.random);
      ImGui::SameLine();
      if (ImGui::Button("Swap sides", ImVec2(150 * scale, 0)))
        rearranged = NetPlay::PadMappingArray{mapping[2], mapping[3],
                                              mapping[0], mapping[1]};
      ImGui::EndDisabled();
      HelpMarker("In Arcade and VS mode, ports 1 and 2 start on the left "
                 "team and ports 3 and 4 on the right. Players can still "
                 "move on the game's POSITION screen. Port 1 runs the menus.");
    } else {
      ImGui::TextDisabled("Port 1 runs the menus. You can still change "
                          "places on the game's POSITION screen.");
    }
    if (rearranged && server) {
      if (server->ArrangeControllers(*rearranged))
        ui.AddEvent("Teams changed");
    }

    // --- Players -----------------------------------------------------------
    ImGui::Spacing();
    if (ImGui::BeginTable("players", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Player", 0, 2.0f);
      ImGui::TableSetupColumn("Connection", 0, 1.6f);
      ImGui::TableSetupColumn("Game files", 0, 1.3f);
      ImGui::TableSetupColumn("Ready", 0, 1.0f);
      ImGui::TableHeadersRow();
      for (const NetPlay::Player &player : players) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        std::string name = player.name;
        if (player.IsHost())
          name += " (host)";
        if (client.IsLocalPlayer(player.pid))
          name += " - you";
        ImGui::TextUnformatted(name.c_str());
        ImGui::TableSetColumnIndex(1);
        if (client.IsLocalPlayer(player.pid) && server) {
          ImGui::TextColored(kGreen, "this computer");
        } else {
          const u32 ping = player.ping;
          const char *word = ping < 40    ? "great"
                             : ping < 80  ? "good"
                             : ping < 150 ? "ok"
                                          : "laggy";
          const ImVec4 colour = ping < 80 ? kGreen : ping < 150 ? kYellow : kRed;
          ImGui::TextColored(colour, "%u ms (%s)", ping, word);
        }
        ImGui::TableSetColumnIndex(2);
        if (player.game_status == NetPlay::SyncIdentifierComparison::SameGame)
          ImGui::TextColored(kGreen, "match");
        else if (player.game_status ==
                 NetPlay::SyncIdentifierComparison::Unknown)
          ImGui::TextDisabled("checking...");
        else
          ImGui::TextColored(kRed, "different game!");
        ImGui::TableSetColumnIndex(3);
        if (player.ready)
          ImGui::TextColored(kGreen, "ready");
        else
          ImGui::TextColored(kYellow, "waiting");
      }
      ImGui::EndTable();
    }
    // The buffer counts controller reads; GameCube games read twice a frame.
    ImGui::TextDisabled("Input delay: about %u ms (grows by itself if a "
                        "connection needs more)",
                        static_cast<unsigned>(ui.BufferSize() * 1000 /
                                              (state.gamecube ? 120 : 60)));

    // --- This computer's controllers --------------------------------------
    ImGui::Spacing();
    {
      std::string ports;
      for (std::size_t i = 0; i < seats.size(); ++i) {
        if (have_local && seats[i].pid == local_player->pid)
          ports += (ports.empty() ? "port " : " and ") + std::to_string(i + 1);
      }
      std::string pads;
      for (const std::string &device : options.controllers)
        pads += (pads.empty() ? "" : ", ") + ControllerLabel(device);
      ImGui::Text("This computer plays %s with: %s",
                  ports.empty() ? "(no port yet)" : ports.c_str(),
                  pads.c_str());
    }
    if (ImGui::TreeNode("Two people on this computer?")) {
      ImGui::TextDisabled("Tick a second controller and this computer gets "
                          "two ports.");
      std::size_t total_assigned = 0;
      for (const NetPlay::Player &player : players)
        total_assigned += player.controller_count;
      const std::size_t local_limit =
          std::min<std::size_t>(4, assigned + 4 - total_assigned);
      for (const ControllerOption &controller : available) {
        bool selected =
            std::ranges::contains(options.controllers, controller.device);
        ImGui::BeginDisabled(lost || (!selected && options.controllers.size() >=
                                                       local_limit));
        if (ImGui::Checkbox(controller.label.c_str(), &selected)) {
          if (selected)
            options.controllers.push_back(controller.device);
          else if (options.controllers.size() > 1)
            std::erase(options.controllers, controller.device);
          frontend_config.controllers = options.controllers;
          frontend_config.controller = options.controllers.front();
          std::string error;
          if (GenerateControllerConfig(runtime_config.user_directory,
                                       options.controllers, &error) &&
              SaveConfig(runtime_config.user_directory, frontend_config,
                         &error)) {
            client.SetLocalControllerCount(
                static_cast<u8>(options.controllers.size()));
            state.auto_ready_sent = false;
          }
        }
        ImGui::EndDisabled();
      }
      ImGui::TreePop();
    }

    // --- Host's gameplay options -----------------------------------------
    if (server) {
      bool marker = frontend_config.neutral_ball_marker;
      if (ImGui::Checkbox("Neutral ball marker for human teams", &marker)) {
        frontend_config.neutral_ball_marker = marker;
        NetPlay::SetGameplayOptions(marker ? kNetplayNeutralBallMarker : 0u);
        std::string ignored;
        SaveConfig(runtime_config.user_directory, frontend_config, &ignored);
      }
      HelpMarker("Your choice applies to everybody's game (the game has to "
                 "run the same on every computer).");
    }

    // --- Start -------------------------------------------------------------
    ImGui::Separator();
    ImGui::BeginDisabled(lost || !have_local);
    if (local_ready) {
      if (ImGui::Button("Wait, I'm not ready", ImVec2(220 * scale, 44 * scale)))
        client.SetReady(false);
    } else {
      if (ImGui::Button("I'm ready", ImVec2(220 * scale, 44 * scale)))
        client.SetReady(true);
    }
    ImGui::EndDisabled();
    std::string waiting_for;
    for (const NetPlay::Player &player : players) {
      if (!player.ready)
        waiting_for += (waiting_for.empty() ? "" : ", ") + player.name;
    }
    const auto filled = std::ranges::count_if(
        mapping, [](NetPlay::PlayerId pid) { return pid != 0; });
    if (server) {
      ImGui::SameLine();
      const bool can_start = !lost && server->CanStart() && ui.Progress().empty();
      ImGui::BeginDisabled(!can_start);
      if (ImGui::Button("Start the game", ImVec2(220 * scale, 44 * scale))) {
        ui.ClearError();
        server->RequestStartGame();
      }
      ImGui::EndDisabled();
    }
    const std::string progress = ui.Progress();
    if (lost)
      ; // the error below says it all
    else if (!progress.empty())
      ImGui::TextColored(kYellow, "%s", progress.c_str());
    else if (filled < 2)
      ImGui::TextColored(kYellow, "Waiting for at least one more player to "
                                  "join...");
    else if (!waiting_for.empty())
      ImGui::TextColored(kYellow, "Waiting for: %s", waiting_for.c_str());
    else if (server)
      ImGui::TextColored(kGreen, "Everyone is ready: press Start the game.");
    else
      ImGui::TextColored(kGreen, "Everyone is ready: waiting for the host to "
                                 "start.");

    const std::string error = ui.Error();
    if (!error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kRed);
      ImGui::TextWrapped("%s", error.c_str());
      ImGui::PopStyleColor();
    }
    for (const std::string &event : ui.Events())
      ImGui::TextDisabled("%s", event.c_str());
    if (lost) {
      if (ImGui::Button("Close", ImVec2(160 * scale, 40 * scale))) {
        ImGui::End();
        window.Present({});
        return LobbyResult::Leave;
      }
    } else {
      ImGui::TextDisabled("Closing this window leaves the lobby%s.",
                          server ? " and ends it for everyone" : "");
    }
    ImGui::End();

    std::string shot;
    if (!state.test.shot_prefix.empty() && state.shot_index < 40 &&
        now - state.last_shot >= std::chrono::seconds(2)) {
      state.last_shot = now;
      shot = state.test.shot_prefix + "-" + std::to_string(state.shot_index++) +
             ".bmp";
    }
    window.Present(shot);
  }
}
} // namespace

int RunNetplayLobby(RuntimeConfig runtime_config, ConfigResult frontend_config,
                    NetplayOptions options, NetplaySessionHooks hooks) {
  if (options.controllers.empty())
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);

  std::cerr << "netplay: initializing Dolphin services\n";
  UICommon::SetUserDirectory(runtime_config.user_directory.string());
  UICommon::Init();
  detail::SetExternalUICommon(true);

  Config::SetBase(Config::MAIN_CPU_THREAD, true);
  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  Config::SetBase(Config::NETPLAY_SAVEDATA_LOAD, true);
  Config::SetBase(Config::NETPLAY_SAVEDATA_WRITE, true);
  Config::SetBase(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII, false);
  Config::SetBase(Config::NETPLAY_SYNC_CODES, false);
  Config::SetBase(Config::NETPLAY_STRICT_SETTINGS_SYNC, true);
  Config::SetBase(Config::NETPLAY_NETWORK_MODE, std::string("fixeddelay"));
  Config::SetBase(Config::NETPLAY_USE_INDEX, false);
  // The host's language goes to everybody with the game settings.
  const int language = GameCubeLanguageValue(frontend_config.language);
  if (language >= 0)
    Config::SetBase(Config::MAIN_GC_LANGUAGE, language);

  auto game = std::make_shared<UICommon::GameFile>(
      (runtime_config.game_root / "sys/main.dol").string());
  if (!game->IsValid()) {
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);
  }

  const GameInspectResult inspected = InspectGame(runtime_config.game_root);
  if (!inspected) {
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);
  }
  std::cerr << "netplay: validating compatibility\n";
  NetPlay::SetCompatibilityFingerprint(
      CompatibilityFingerprint(runtime_config, *inspected.metadata));
  std::cerr << "netplay: compatibility validated\n";
  NetPlay::SetGameplayOptions(
      frontend_config.neutral_ball_marker ? kNetplayNeutralBallMarker : 0u);

  LobbyState state;
  state.test = ReadTestHooks();
  state.gamecube = inspected.metadata->platform == GamePlatform::GameCube;
  const std::string game_title =
      runtime_config.window_title && !runtime_config.window_title->empty()
          ? *runtime_config.window_title
          : inspected.metadata->game_name;
  state.title = game_title + " - online lobby";

  const NetplayAddress address =
      ParseNetplayAddress(options.address, options.port);
  if (options.role == NetplayRole::Join) {
    if (!address.ok) {
      std::cerr << "netplay: '" << options.address
                << "' is not a host address\n";
      detail::SetExternalUICommon(false);
      UICommon::Shutdown();
      return static_cast<int>(NetplayExitCode::InvalidConfiguration);
    }
    options.address = address.host;
    options.port = address.port;
  }

  SessionUI ui(game);
  const NetPlay::NetTraversalConfig direct{};
  std::unique_ptr<NetPlay::NetPlayServer> server;
  std::unique_ptr<NetPlay::NetPlayClient> client;
  if (options.role == NetplayRole::Host) {
    std::cerr << "netplay: opening host on UDP port " << options.port << '\n';
    ui.SetHosting(true);
    server = std::make_unique<NetPlay::NetPlayServer>(options.port, false, &ui,
                                                      direct);
    if (!server->is_connected) {
      std::cerr << "netplay: UDP port " << options.port
                << " is not available (another lobby open?)\n";
      server.reset();
      detail::SetExternalUICommon(false);
      UICommon::Shutdown();
      return static_cast<int>(NetplayExitCode::Failed);
    }
    server->SetGameCubeControllers(state.gamecube);
    server->SetHostInputAuthority(false);
    server->SetAdaptiveBuffer(options.buffer == "auto");
    if (options.buffer != "auto")
      server->AdjustPadBufferSize(
          static_cast<unsigned int>(std::stoul(options.buffer)));
    server->ChangeGame(game->GetSyncIdentifier(),
                       inspected.metadata->game_name);
    client = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", server->GetPort(), &ui, options.nickname, direct,
        static_cast<u8>(options.controllers.size()));
  } else {
    std::cerr << "netplay: connecting to " << options.address << ':'
              << options.port << '\n';
    client = std::make_unique<NetPlay::NetPlayClient>(
        options.address, options.port, &ui, options.nickname, direct,
        static_cast<u8>(options.controllers.size()));
  }

  if (!client->IsConnected()) {
    const NetPlay::ConnectionError connection_error =
        client->GetConnectionError();
    const std::string error = ui.Error();
    if (!error.empty())
      std::cerr << "netplay: " << error << '\n';
    client.reset();
    server.reset();
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    switch (connection_error) {
    case NetPlay::ConnectionError::VersionMismatch:
      return static_cast<int>(NetplayExitCode::VersionMismatch);
    case NetPlay::ConnectionError::CompatibilityMismatch:
      return static_cast<int>(NetplayExitCode::CompatibilityMismatch);
    case NetPlay::ConnectionError::RoomFull:
      return static_cast<int>(NetplayExitCode::RoomFull);
    case NetPlay::ConnectionError::GameRunning:
      return static_cast<int>(NetplayExitCode::GameRunning);
    case NetPlay::ConnectionError::ServerFull:
      return static_cast<int>(NetplayExitCode::ServerFull);
    case NetPlay::ConnectionError::NameTooLong:
      return static_cast<int>(NetplayExitCode::NicknameRejected);
    case NetPlay::ConnectionError::NoError:
      return static_cast<int>(NetplayExitCode::HostUnavailable);
    }
  }
  ui.SetClient(client.get());
  std::cerr << "netplay: connected; opening lobby\n";

  int result = 0;
  while (true) {
    std::string banner;
    const LobbyResult lobby =
        RunLobbyWindow(runtime_config, frontend_config, options, ui, *client,
                       server.get(), state, &banner);
    if (lobby != LobbyResult::Boot)
      break;

    auto created = Runtime::Create(runtime_config);
    if (!created) {
      std::cerr << "netplay: initialization failed: "
                << created.error->message << '\n';
      result = 1;
      break;
    }
    const u32 gameplay_options = client->GetNetSettings().gameplay_options;
    std::cerr << "netplay: game options from the host: 0x" << std::hex
              << gameplay_options << std::dec << '\n';
    if (hooks.before_run)
      hooks.before_run(*created.runtime, frontend_config, gameplay_options,
                       banner);
    ui.SetRuntime(created.runtime.get());
    const RuntimeRunResult run_result = created.runtime->Run();
    ui.SetRuntime(nullptr);
    if (hooks.after_run)
      hooks.after_run();
    client->Stop();
    client->StopGame();
    created.runtime.reset();
    ++state.games_played;
    std::cerr << "netplay: game " << state.games_played << " ended\n";
    if (run_result.error) {
      std::cerr << "netplay: runtime failed: " << run_result.error->message
                << '\n';
      result = 1;
      break;
    }
    if (state.test.leave_after > 0 &&
        state.games_played >= state.test.leave_after)
      break;
    // Back to the lobby, connected or not: a lost connection is explained
    // there instead of the window just vanishing.
  }

  client.reset();
  server.reset();
  detail::SetExternalUICommon(false);
  UICommon::Shutdown();
  return result;
}
} // namespace moderngekko::frontend
