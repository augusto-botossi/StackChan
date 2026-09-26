#include "hal.h"
#include "board/hal_bridge.h"
#include <assets/lang_config.h>
#include <stackchan/stackchan.h>
#include <mooncake_log.h>
#include <fmt/format.h>
#include <algorithm>
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

        bool wants_location = lower.find("where") != std::string::npos ||
                               lower.find("location") != std::string::npos ||
                               lower.find("gps") != std::string::npos;
        bool wants_environment = lower.find("temperature") != std::string::npos ||
                                  lower.find("weather") != std::string::npos ||
                                  lower.find("humidity") != std::string::npos ||
                                  lower.find("air quality") != std::string::npos;

        if (wants_location && GetHAL().getGps()) {
            auto fix = GetHAL().getGps()->getLastFix();
            if (fix.valid) {
                augmented += fmt::format(
                    " [Current GPS reading: latitude {:.6f}, longitude {:.6f}, altitude {:.1f}m, speed {:.1f}km/h, {} satellites]",
                    fix.latitude, fix.longitude, fix.altitudeM, fix.speedKmh, fix.satellites);
            } else {
                augmented += " [No GPS fix currently available]";
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
                augmented += fmt::format(
                    " [Current sensor reading: temperature {:.1f}C, humidity {:.1f}%, pressure {:.1f}hPa, "
                    "air quality index (IAQ) {:.0f} (0-500 scale, lower is better; accuracy {}/3, "
                    "where 0 means still calibrating), estimated CO2 equivalent {:.0f}ppm]",
                    r.temperatureC, r.humidityPct, r.pressureHpa, r.iaq, r.iaqAccuracy, r.co2EquivalentPpm);
            } else {
                augmented += " [No environment sensor reading currently available]";
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