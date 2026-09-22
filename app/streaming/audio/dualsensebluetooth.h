#pragma once

#include "SDL_compat.h"

#include <Limelight.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

struct DualSenseOutputReport;
struct OpusEncoder;

class DualSenseBluetoothRenderer
{
public:
    DualSenseBluetoothRenderer();
    ~DualSenseBluetoothRenderer();

    void setController(int controllerNumber, SDL_GameController* controller, bool bluetooth);
    bool isActive() const;
    void submit(const LI_DS5_HAPTICS_PCM_FRAME& frame);
    void submit(const LI_DS5_AUDIO_PCM_FRAME& frame);
    void setRumble(uint16_t controllerNumber, uint16_t lowFrequency, uint16_t highFrequency);
    void setLed(uint16_t controllerNumber, uint8_t red, uint8_t green, uint8_t blue);
    void setAdaptiveTriggers(uint16_t controllerNumber, const DualSenseOutputReport* report);
    void reset();

private:
    bool sendWakeLocked();
    bool sendStateLocked();
    bool sendMediaLocked();
    bool sendRawReportLocked(const uint8_t* report, std::size_t size);
    void resetStreamLocked();
    void consumeFrameLocked(int16_t speakerLeft, int16_t speakerRight, int16_t hapticLeft,
                            int16_t hapticRight);
    int8_t filterLaneLocked(const std::array<double, 127>& lane) const;
    static uint32_t bluetoothCrc(const uint8_t* data, std::size_t size);

    mutable std::mutex m_Mutex;
    SDL_GameController* m_Controller = nullptr;
    int m_ControllerNumber = -1;
    bool m_Active = false;
    bool m_WakeSent = false;
    uint8_t m_Sequence = 0;
    uint8_t m_PacketCounter = 0;
    uint8_t m_LeftRumble = 0;
    uint8_t m_RightRumble = 0;
    uint8_t m_Red = 0;
    uint8_t m_Green = 80;
    uint8_t m_Blue = 255;
    uint8_t m_PlayerLeds = 0x04;
    std::array<uint8_t, 11> m_LeftTrigger = {};
    std::array<uint8_t, 11> m_RightTrigger = {};
    std::array<double, 127> m_LeftLane = {};
    std::array<double, 127> m_RightLane = {};
    std::array<double, 127> m_Filter = {};
    std::array<int8_t, 64> m_HapticBlock = {};
    std::array<int16_t, 512 * 2> m_SpeakerPcm = {};
    std::array<uint8_t, 200> m_SpeakerOpus = {};
    OpusEncoder* m_Encoder = nullptr;
    int m_SpeakerFrames = 0;
    int m_RingPosition = 0;
    int m_DecimationPhase = 0;
    int m_HapticPosition = 0;
    int64_t m_ExpectedSequence = -1;
};
