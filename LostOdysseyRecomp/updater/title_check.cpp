#include "title_check.h"

#if defined(__ANDROID__)
#include "update.h"
#include "../os/logger.h"
#include "../os/thread_name.h"
#include "../settings/config.h"
#include "../version.h"

#include <SDL3/SDL.h>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace updater::title_check {
namespace {
std::mutex g_mutex;
Machine g_machine;
std::atomic<bool> g_settled{false};
std::string g_tag, g_url;
uint32_t g_language = 0;

void Settle()
{
    if (g_machine.Settled()) g_settled.store(true, std::memory_order_release);
}

void CheckThread()
{
    os::SetCurrentThreadName("Update Check");
    StartupOptions options;
    options.currentVersion = lo_version::Source;
    options.automaticUpdates = true;
    std::string tag, url;
    const auto result = CheckLatestAndroidRelease(options, tag, url);
    const bool available = result.status == StartupStatus::ExternalUpdateAvailable && !url.empty();
    LOG_INFO("update check (title screen, own thread): {} {}", StatusName(result.status), result.detail);
    std::lock_guard lock(g_mutex);
    g_tag = std::move(tag);
    g_url = std::move(url);
    g_machine.CheckFinished(available, !available && result.status != StartupStatus::UpToDate);
    Settle();
}

void PromptThread()
{
    os::SetCurrentThreadName("Update Prompt");
    std::string tag, url;
    uint32_t language = 0;
    {
        std::lock_guard lock(g_mutex);
        tag = g_tag; url = g_url; language = g_language;
    }
    // Stable UI language IDs: 1 Traditional Chinese, 4 Simplified Chinese.
    const bool chinese = language == 1 || language == 4;
    const std::string title = chinese ? "有可用更新" : "Update available";
    const std::string message = chinese
        ? "Lost Odyssey Recompiled " + tag + " 已發布。要在瀏覽器中下載嗎？"
        : "Lost Odyssey Recompiled " + tag + " is available. Download it in the browser now?";
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, chinese ? "稍後" : "Later"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, chinese ? "下載" : "Download"},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.title = title.c_str();
    box.message = message.c_str();
    box.numbuttons = 2;
    box.buttons = buttons;
    int pressed = 0;
    if (!SDL_ShowMessageBox(&box, &pressed))
        LOG_WARNING("update check: the update dialog could not be shown: {}", SDL_GetError());
    else if (pressed == 1) {
        if (SDL_OpenURL(url.c_str())) LOG_INFO("update check: opened {}", url);
        else LOG_WARNING("update check: could not open {}: {}", url, SDL_GetError());
    } else
        LOG_INFO("update check: {} declined at the title screen", tag);
    std::lock_guard lock(g_mutex);
    g_machine.PromptFinished();
    Settle();
}
}

bool Enabled()
{
    static const bool enabled = EnabledFor(std::getenv("LO_UPDATE_AT_TITLE"));
    return enabled;
}

void OnTitleIdle()
{
    if (g_settled.load(std::memory_order_acquire)) return;
    Action action = Action::None;
    {
        std::unique_lock lock(g_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        // Idle or Available: the only states with something to start.
        if (g_machine.state != State::Idle && g_machine.state != State::Available) return;
        const char* disabled = std::getenv("LO_NO_UPDATE");
        const bool blocked = disabled && std::string_view(disabled) != "0";
        const auto config = settings::GetConfig();
        g_language = config.uiLanguage;
        if (!Enabled() || blocked) {
            g_machine.state = State::Done;
            Settle();
            return;
        }
        action = g_machine.OnTitleIdle(true, config.automaticUpdates);
    }
    if (action == Action::StartCheck) std::thread(CheckThread).detach();
    else if (action == Action::StartPrompt) std::thread(PromptThread).detach();
}
}
#else
namespace updater::title_check {
bool Enabled() { return false; }
void OnTitleIdle() {}
}
#endif
