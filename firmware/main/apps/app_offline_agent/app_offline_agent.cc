/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_offline_agent.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

AppOfflineAgent::AppOfflineAgent()
{
    // Configure App name
    setAppInfo().name = "OFFLINE.AGENT";
    // Configure App icon
    // NOTE: reusing icon_ai_agent.bin as a placeholder - there's no
    // dedicated icon asset for this app yet. Swap this out once one
    // exists (a real design asset, not something to generate here).
    static auto icon  = assets::get_image("icon_ai_agent.bin");
    setAppInfo().icon = (void*)&icon;
    // Configure App theme color - distinct from AI.AGENT's 0x33CC99
    // green, so the two are visually distinguishable in the launcher.
    static uint32_t theme_color = 0x3399CC;
    setAppInfo().userData       = (void*)&theme_color;
}

// Called when the App is installed
void AppOfflineAgent::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

// Called when the App is opened
// You can construct UI, initialize operations, etc. here
void AppOfflineAgent::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    // Request to start the offline (Module LLM native pipeline) agent.
    // Same one-way transition as AI.AGENT/requestXiaozhiStart() - the
    // whole mooncake framework gets torn down once this fires (see
    // main.cpp), so this app can't be backed out of without a reboot,
    // matching the existing AI.AGENT behavior exactly.
    GetHAL().requestOfflineAgentStart();
}

// Called repeatedly while the App is running
void AppOfflineAgent::onRunning()
{
}

// Called when the App is closed
// You can destroy UI, release resources, etc. here
void AppOfflineAgent::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
}
