/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>

/**
 * @brief Derived App
 *
 */
class AppOfflineAgent : public mooncake::AppAbility {
public:
    AppOfflineAgent();

    // Override lifecycle callbacks
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};
