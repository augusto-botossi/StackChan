#include "hal.h"
#include "board/hal_bridge.h"
#include <assets/lang_config.h>
#include <stackchan/stackchan.h>
#include <mooncake_log.h>
#include <fmt/format.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>
#include <apps/common/common.h>
#include <assets/assets.h>
#include <settings.h>

static const std::string_view _tag = "HAL-OfflineAgent";

// ---- name memory ----
// The model has no memory between turns, so the firmware keeps the user's name and attaches
// "My name is <name>." (about 6 estimated tokens) in front of a question ABOUT the name ("what is my
// name", "who am I"...), not of every question: attached to all of them it made her greet the user
// ("Hello Augusto!") in nearly every answer. The name is
// saved in flash (NVS namespace "becky", key "user_name") so it survives a reboot; it is forgotten
// on "forget my name".
// Wording and position were measured (probe_name_memory.py --wordings, 8 trials each) on "What is my
// name?": "My name is X." before the question answered it 8 of 8 times; "The user is X." after the
// question (the first version) only 2 of 8 - the rest were greetings ("Hello, Augusto! How can I
// assist you today?"); "The user's name is X." scored 0 of 8 either way. Written as the user's own
// words the model answers "Your name is X.". The module trims sensor sentences before it touches
// this prefix (see handleAsrResult()).
static std::string g_user_name;
static bool g_user_name_loaded = false;

static constexpr const char* kNameNvsNamespace = "becky";
static constexpr const char* kNameNvsKey = "user_name";

static void load_user_name_once()
{
    if (g_user_name_loaded) return;
    g_user_name_loaded = true;
    Settings settings(kNameNvsNamespace, false);  // read-only: no namespace yet simply gives ""
    g_user_name = settings.GetString(kNameNvsKey, "");
    if (!g_user_name.empty()) {
        mclog::tagInfo(_tag, "Restored the user's name from flash: {}", g_user_name);
    }
}

// An empty name is stored as "" rather than erased: Settings::EraseKey() does not mark the handle
// dirty, so the erase would never be committed.
static void save_user_name(const std::string& name)
{
    Settings settings(kNameNvsNamespace, true);
    settings.SetString(kNameNvsKey, name);
}

// Looks for "my name is X" / "call me X" and returns X as a single capitalised word, or "".
// Deliberately NOT "I am X" / "I'm X": that matches "I'm hungry" and would store "Hungry".
static std::string extract_spoken_name(const std::string& text)
{
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    static const char* const kMarkers[] = {"my name is ", "my name's ", "call me "};
    for (const char* marker : kMarkers) {
        size_t at = lower.find(marker);
        if (at == std::string::npos) continue;
        size_t i = at + std::string(marker).size();
        std::string word;
        while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) {
            word.push_back(text[i++]);
        }
        if (word.size() < 2 || word.size() > 20) continue;
        // A name with an accent or other non-ASCII letter would be cut short ("Zoë" -> "Zo"): skip it.
        if (i < text.size() && static_cast<unsigned char>(text[i]) >= 0x80) continue;
        std::string w = word;
        std::transform(w.begin(), w.end(), w.begin(), ::tolower);
        // Words that follow the marker without being a name ("call me back", "my name is not ...").
        static const char* const kNotNames[] = {"a",   "an",   "the",  "not",   "back", "later", "when",
                                                "if",  "again", "now", "please", "maybe", "also"};
        bool rejected = false;
        for (const char* bad : kNotNames) {
            if (w == bad) rejected = true;
        }
        if (rejected) continue;
        word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
        for (size_t k = 1; k < word.size(); ++k) {
            word[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(word[k])));
        }
        return word;
    }
    return "";
}

void Hal::registerOfflineAgentCallbacks(OfflineAgentModule* agent)
{
    agent->onStateChange([](OfflineAgentModule::State oldState, OfflineAgentModule::State newState) {
        using State = OfflineAgentModule::State;
        if (newState == State::Listening || newState == State::FollowUp) {
            hal_bridge::set_display_status(Lang::Strings::LISTENING);
        } else if (newState == State::Processing) {
            // Blue (matches SPEAKING's color) but deliberately NOT via
            // SetStatus(SPEAKING) - that also starts mouth movement,
            // which shouldn't animate yet since no audio is playing
            // during Processing. Direct LED-only call instead.
            GetHAL().setRgbColor(0, 0, 0, 50);
            GetHAL().refreshRgb();
        } else if (newState == State::Speaking) {
            hal_bridge::set_display_status(Lang::Strings::SPEAKING);
        } else if (newState == State::Idle) {
            GetHAL().setRgbColor(0, 0, 0, 0);
            GetHAL().refreshRgb();
            hal_bridge::restore_idle_motion();
            hal_bridge::clear_chat_messages();
        }
        // set_display_status() above ends in StackChanAvatarDisplay::SetStatus(), which sets the
        // "xiaozhi idle" flag to false for every status except STANDBY - and the avatar update task
        // (_stackchan_update_task in hal.cpp) waits an extra 100 ms per cycle while that flag is false.
        // Becky never reports STANDBY, so after the first wake word the idle movement ran at ~8 updates
        // per second instead of ~50 and looked choppy next to the AI Assistant. The offline app has no
        // network/audio load on this CPU that the throttle was meant to protect, so keep it off.
        hal_bridge::set_xiaozhi_idle(true);
    });

    agent->onSpeechText([](const std::string& text) {
        hal_bridge::set_chat_message("assistant", text.c_str());
    });

    agent->onBeforeLlmInference([](const std::string& asr_text) -> std::string {
        std::string lower = asr_text;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        std::string augmented = asr_text;

        // ---- name memory (see extract_spoken_name) ----
        std::string name_prefix;  // goes in FRONT of the question, see the comment above g_user_name
        load_user_name_once();
        if (lower.find("forget my name") != std::string::npos) {
            if (!g_user_name.empty()) {
                g_user_name.clear();
                save_user_name(g_user_name);
                mclog::tagInfo(_tag, "Forgot the user's name");
            }
        } else {
            std::string spoken = extract_spoken_name(asr_text);
            if (!spoken.empty() && spoken != g_user_name) {
                g_user_name = spoken;
                save_user_name(g_user_name);  // only when it changed: spares the flash
                mclog::tagInfo(_tag, "Remembering the user's name: {}", g_user_name);
            }
        }
        // Not when the user is introducing themselves right now: the question already says it.
        const bool introducing = !extract_spoken_name(asr_text).empty();
        // And only when the question is about the name itself (see the comment above g_user_name).
        bool asks_name = false;
        for (const char* phrase : {"my name", "who am i", "who i am", "know me", "remember me", "call me",
                                   "what am i called"}) {
            if (lower.find(phrase) != std::string::npos) {
                asks_name = true;
                break;
            }
        }
        if (!g_user_name.empty() && !introducing && asks_name) {
            // The ASR text normally starts with a space already: add one only if it does not, so the
            // prompt keeps the single-space shape the probe measured.
            name_prefix = "My name is " + g_user_name + ".";
            if (asr_text.empty() || asr_text[0] != ' ') name_prefix += " ";
        }

        // PROMPT LENGTH IS THE CRITICAL CONSTRAINT. The model's prefill window is 128 tokens, system
        // prompt included, and a longer prompt makes it derail from the very first token (measured:
        // 40 of 40 derailments at 131-133 tokens, 0 of 10 at 127). Qwen also spends one token per
        // digit, so a reading is expensive. Therefore: attach ONLY what was asked about, in short
        // plain sentences (no square brackets, no units glued to numbers). The module trims anything
        // that still does not fit.
        auto contains_any = [&lower](std::initializer_list<const char*> words) {
            for (const char* w : words) {
                if (lower.find(w) != std::string::npos) return true;
            }
            return false;
        };

        // ---- position (latitude/longitude; altitude and speed only if asked) ----
        // "position"/"coordinates" were once missing here and the model invented an address;
        // a bare "where" matched unrelated speech ("...in space where Saturn formed").
        bool wants_location = contains_any({"where am", "where are", "where is", "location", "gps", "position",
                                            "coordinates", "latitude", "longitude", "altitude", "elevation",
                                            "how high", "located"});
        // "height" counts too ("how high we are" was once heard as "tell me or height"), except when it is
        // about Becky herself.
        bool wants_altitude = contains_any({"altitude", "how high", "elevation", "above sea", "height"}) &&
                              !contains_any({"your height", "how tall"});
        wants_location = wants_location || wants_altitude;
        bool wants_speed = contains_any({"speed", "how fast", "moving"});

        // ---- environment (only the readings that were asked for) ----
        bool wants_weather = contains_any({"weather", "conditions"});
        // "is it too hot?" used to get no reading at all and the model answered without data.
        bool wants_temp = wants_weather || contains_any({"temperature", "how hot", "how cold", "how warm", "too hot",
                                                          "too cold", "is it hot", "is it cold", "is it warm"});
        bool wants_humidity = wants_weather || contains_any({"humidity", "humid"});
        bool wants_pressure = wants_weather || contains_any({"pressure"});
        bool wants_air = contains_any({"air quality", "how is the air", "carbon dioxide", "co2", "breathe"});
        bool wants_environment = wants_temp || wants_humidity || wants_pressure || wants_air;

        if (wants_location && GetHAL().getGps()) {
            auto fix = GetHAL().getGps()->getLastFix();
            if (fix.valid) {
                // Spelled-out words: the dense "latitude 50.257415, longitude 8.642969, altitude
                // 201.4m" form was ignored by the model even though it was attached. 4 decimals
                // (~11 m) is plenty for speech.
                std::string pos = fmt::format("latitude is {:.4f} degrees {}, longitude is {:.4f} degrees {}",
                                              std::fabs(fix.latitude), fix.latitude >= 0 ? "north" : "south",
                                              std::fabs(fix.longitude), fix.longitude >= 0 ? "east" : "west");
                if (wants_speed) {
                    pos += fmt::format(", speed is {:.1f} kilometers per hour", fix.speedKmh);
                }
                augmented += " Current position: " + pos + ".";
                if (wants_altitude) {
                    // A separate sentence that says what "height" means here: "how high we are" was once
                    // answered as if it were about Becky's own height.
                    augmented += fmt::format(" Our altitude, our height above sea level, is {:.0f} meters.",
                                             fix.altitudeM);
                }
            } else {
                augmented += " No GPS fix is currently available.";
            }
        }

        if (wants_environment && GetHAL().getEnvSensor()) {
            auto r = GetHAL().getEnvSensor()->getLastReading();
            if (r.ok) {
                std::vector<std::string> parts;
                if (wants_temp) {
                    parts.push_back(fmt::format("temperature is {:.1f} degrees Celsius", r.temperatureC));
                }
                if (wants_humidity) {
                    parts.push_back(fmt::format("humidity is {:.0f} percent", r.humidityPct));
                }
                if (wants_pressure) {
                    parts.push_back(fmt::format("pressure is {:.0f} hectopascals", r.pressureHpa));
                }
                if (wants_air) {
                    // "where lower is better": the IAQ scale is not self-explanatory, and this lets
                    // the model give a verdict itself instead of us hardcoding thresholds.
                    parts.push_back(fmt::format("air quality index is {:.0f} out of 500 where lower is better", r.iaq));
                    parts.push_back(fmt::format("carbon dioxide level is about {:.0f} parts per million",
                                                r.co2EquivalentPpm));
                }
                std::string joined;
                for (const auto& part : parts) {
                    if (!joined.empty()) joined += ", ";
                    joined += part;
                }
                augmented += " Current conditions: " + joined + ".";
            } else {
                augmented += " No environment sensor reading is currently available.";
            }
        }

        return name_prefix + augmented;
    });
}

void Hal::constructAndStartOfflineAgent()
{
    offline_agent_ = std::make_unique<OfflineAgentModule>(UART_NUM_0, 115200, 17, 18);
    registerOfflineAgentCallbacks(offline_agent_.get());
    if (!offline_agent_->start()) {
        mclog::tagError(_tag, "OfflineAgentModule failed to start");
    }
}

void Hal::startOfflineAgent()
{
    mclog::tagInfo(_tag, "start offline agent");

    auto& motion = GetStackChan().motion();
    motion.setAutoAngleSyncEnabled(true);
    motion.setAutoTorqueReleaseEnabled(true);

    tools::on_reminder_triggered().clear();
    tools::on_reminder_triggered().connect([](int id, std::string_view msg) {
        mclog::tagInfo(_tag, "reminder triggered: id: {}, msg: {}", id, msg);
        {
            LvglLockGuard lock;
            auto& avatar = GetStackChan().avatar();
            avatar.addDecorator(std::make_unique<view::ReminderView>(lv_screen_active(), msg));
        }
        hal_bridge::app_play_sound(OGG_NEW_NOTIFICATION);
    });

    xTaskCreatePinnedToCore(_stackchan_update_task, "stackchan", 4096, NULL, 3, NULL, 1);

    hal_bridge::ensure_avatar_created();
    hal_bridge::set_xiaozhi_idle(true);

    constructAndStartOfflineAgent();  // the actual call, this time

    xTaskCreate([](void*) {
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(20));
            GetHAL().updateOfflineAgent();
        }
    }, "offline_agent_poll", 8192, NULL, 3, NULL);
}