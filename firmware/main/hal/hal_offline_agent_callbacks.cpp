#include "hal.h"
#include "board/hal_bridge.h"
#include <assets/lang_config.h>
#include <stackchan/stackchan.h>
#include <mooncake_log.h>
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
#include <apps/common/common.h>
#include <assets/assets.h>

static const std::string_view _tag = "HAL-OfflineAgent";

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
    });

    agent->onSpeechText([](const std::string& text) {
        hal_bridge::set_chat_message("assistant", text.c_str());
    });

    agent->onBeforeLlmInference([](const std::string& asr_text) -> std::string {
        std::string lower = asr_text;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        std::string augmented = asr_text;

        // Confirmed real: "tell me our position" and "give our current coordinates" matched
        // none of the old keywords (where/location/gps), so no GPS reading was attached and
        // the model invented an address. Broader lists below; a stray match (e.g. "peer
        // pressure") just attaches a reading the model ignores.
        auto contains_any = [&lower](std::initializer_list<const char*> words) {
            for (const char* w : words) {
                if (lower.find(w) != std::string::npos) return true;
            }
            return false;
        };
        bool wants_location = contains_any({"where am", "where are", "where is", "location", "gps", "position", "coordinates",
                                            "latitude", "longitude", "altitude", "elevation",
                                            "how high", "located"});
        bool wants_environment = contains_any({"temperature", "weather", "humidity", "air quality",
                                               "pressure", "carbon dioxide", "co2", "how hot",
                                               "how cold"});

        if (wants_location && GetHAL().getGps()) {
            auto fix = GetHAL().getGps()->getLastFix();
            if (fix.valid) {
                // Natural-language phrasing, like the environment reading: the dense
                // "latitude 50.257415, longitude 8.642969, altitude 201.4m ..." form was
                // IGNORED by the model ("I do not yet have the current location
                // information") although it was attached, while the spelled-out
                // environment reading is read back correctly. 4 decimals (~11 m) is
                // plenty for speech.
                augmented += fmt::format(
                    " Current position: latitude is {:.4f} degrees {}, longitude is {:.4f} degrees {}, "
                    "altitude is {:.0f} meters above sea level, speed is {:.1f} kilometers per hour, "
                    "and {} satellites are in view.",
                    std::fabs(fix.latitude), fix.latitude >= 0 ? "north" : "south",
                    std::fabs(fix.longitude), fix.longitude >= 0 ? "east" : "west",
                    fix.altitudeM, fix.speedKmh, fix.satellites);
            } else {
                augmented += " No GPS fix is currently available.";
            }
        }

        if (wants_environment && GetHAL().getEnvSensor()) {
            auto r = GetHAL().getEnvSensor()->getLastReading();
            if (r.ok) {
                // Includes IAQ/CO2 now too - the original only had
                // temperature/humidity/pressure, meaning an air-quality
                // question got the keyword trigger right but no actual
                // air-quality data. "lower is better" note included since,
                // unlike temperature/humidity/pressure, IAQ's scale isn't
                // self-explanatory - lets the LLM give a genuine verdict
                // itself rather than us hardcoding our own good/bad
                // thresholds (risking getting BSEC's own categorization wrong).
                // Natural-language phrasing (spelled-out words, no
                // glued units like "24.9C", no nested parentheticals,
                // no slash notation like "0/3") - tried after a real
                // regression, though the evidence is mixed: an earlier
                // log showed this exact same dense/glued format working
                // fine when NOT near a reset, suggesting post-reset
                // context fragility is the more likely root cause, not
                // this formatting itself. Low-risk to try regardless.
                augmented += fmt::format(
                    " Current conditions: temperature is {:.1f} degrees Celsius, humidity is {:.0f} percent, "
                    "pressure is {:.0f} hectopascals, air quality index is {:.0f} out of 500 where lower is "
                    "better, carbon dioxide level is about {:.0f} parts per million.",
                    r.temperatureC, r.humidityPct, r.pressureHpa, r.iaq, r.co2EquivalentPpm);
            } else {
                augmented += " No environment sensor reading is currently available.";
            }
        }

        return augmented;
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