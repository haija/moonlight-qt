#pragma once

#include <memory>

#include <Limelight.h>
#include "SDL_compat.h"

struct DualSenseOutputReport;

class DualSenseHapticsRenderer
{
public:
    DualSenseHapticsRenderer();
    ~DualSenseHapticsRenderer();

    DualSenseHapticsRenderer(const DualSenseHapticsRenderer&) = delete;
    DualSenseHapticsRenderer& operator=(const DualSenseHapticsRenderer&) = delete;

    static bool isAvailable();
    void submit(const LI_DS5_HAPTICS_PCM_FRAME& frame);
    void submit(const LI_DS5_AUDIO_PCM_FRAME& frame);
    void setControllerTarget(int controllerNumber, SDL_GameController* controller = nullptr,
                             bool bluetooth = false);
    bool isBluetoothActive() const;
    void setBluetoothRumble(uint16_t controllerNumber, uint16_t lowFrequency,
                            uint16_t highFrequency);
    void setBluetoothLed(uint16_t controllerNumber, uint8_t red, uint8_t green, uint8_t blue);
    void setBluetoothAdaptiveTriggers(uint16_t controllerNumber,
                                      const DualSenseOutputReport* report);
    void reset();
    bool submit(const LI_DS5_HAPTICS_IR_FRAME_V2& frame, bool& startedNative);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
