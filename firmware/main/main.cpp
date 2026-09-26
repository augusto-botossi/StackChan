/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    const bool skip_mooncake =
        GetHAL().getXiaozhiConfig().startAiAgentOnBoot && GetHAL().getWarmRebootTarget() < 0;
    // NOTE: deliberately no equivalent "start offline agent on boot"
    // skip - offline mode is always chosen explicitly through the
    // launcher, never auto-started. Module LLM boots independently of
    // CoreS3 regardless of this flag, so an auto-boot skip here
    // wouldn't solve anything Module LLM-related, it would just remove
    // the ability to choose cloud vs. offline each session.

    if (!skip_mooncake) {
        // Install apps
        GetMooncake().installApp(std::make_unique<AppLauncher>());
        GetMooncake().installApp(std::make_unique<AppAiAgent>());
        GetMooncake().installApp(std::make_unique<AppOfflineAgent>());
        GetMooncake().installApp(std::make_unique<AppAvatar>());
        GetMooncake().installApp(std::make_unique<AppEspnowControl>());
        GetMooncake().installApp(std::make_unique<AppAppCenter>());
        GetMooncake().installApp(std::make_unique<AppEzdata>());
        GetMooncake().installApp(std::make_unique<AppDance>());
        GetMooncake().installApp(std::make_unique<AppSetup>());

        // Main loop
        while (1) {
            GetHAL().feedTheDog();
            GetHAL().updateHeapStatusLog();

            GetMooncake().update();

            if (GetHAL().isXiaozhiStartRequested() || GetHAL().isOfflineAgentStartRequested()) {
                break;
            }
        }

        // Uninstall all apps and destroy mooncake
        GetMooncake().uninstallAllApps();
        DestroyMooncake();
    }

    // Exactly one of these fires - whichever flag caused the loop above
    // to break (or, if skip_mooncake was true, isOfflineAgentStartRequested()
    // is guaranteed false since that path never installed/opened any
    // app, so this correctly falls through to Xiaozhi as before).
    if (GetHAL().isOfflineAgentStartRequested()) {
        // Start offline agent, never returns
        GetHAL().startOfflineAgent();
    } else {
        // Start xiaozhi, never returns
        GetHAL().startXiaozhi();
    }
}