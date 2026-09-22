#include "dualsensebluetooth.h"

#include "streaming/input/input.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr uint32_t SampleRate = 48000;
constexpr uint32_t BytesPerFrame = 2 * sizeof(int16_t);

bool matchesController(int selected, uint16_t incoming)
{
    return selected >= 0 && selected == static_cast<int>(incoming);
}
}

DualSenseBluetoothRenderer::DualSenseBluetoothRenderer()
{
    const double middle = (m_Filter.size() - 1) / 2.0;
    const double normalizedCutoff = 1400.0 / SampleRate;
    double sum = 0.0;
    for (std::size_t i = 0; i < m_Filter.size(); i++) {
        const double x = static_cast<double>(i) - middle;
        const double sinc = x == 0.0 ? 2.0 * normalizedCutoff
                                     : std::sin(2.0 * Pi * normalizedCutoff * x) / (Pi * x);
        const double window = 0.42 - 0.5 * std::cos(2.0 * Pi * i / (m_Filter.size() - 1)) +
                              0.08 * std::cos(4.0 * Pi * i / (m_Filter.size() - 1));
        m_Filter[i] = sinc * window;
        sum += m_Filter[i];
    }
    for (double& coefficient : m_Filter) {
        coefficient /= sum;
    }
}

uint32_t DualSenseBluetoothRenderer::bluetoothCrc(const uint8_t* data, std::size_t size)
{
    uint32_t crc = 0xffffffffU;
    auto update = [&crc](uint8_t value) {
        crc ^= value;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
        }
    };
    update(0xa2);
    for (std::size_t i = 0; i < size; i++) {
        update(data[i]);
    }
    return crc ^ 0xffffffffU;
}

bool DualSenseBluetoothRenderer::sendRawReportLocked(const uint8_t* report, std::size_t size)
{
    static constexpr uint8_t Magic[] = { 'A', 'R', 'T', 'B', 'T', '0', '1', 0 };
    if (m_Controller == nullptr || report == nullptr || size == 0) {
        return false;
    }

    QByteArray envelope(static_cast<int>(sizeof(Magic) + size), 0);
    std::memcpy(envelope.data(), Magic, sizeof(Magic));
    std::memcpy(envelope.data() + sizeof(Magic), report, size);
    return SDL_GameControllerSendEffect(m_Controller, envelope.constData(), envelope.size()) == 0;
}

bool DualSenseBluetoothRenderer::sendWakeLocked()
{
    std::array<uint8_t, 142> report = {};
    report[0] = 0x32;
    report[1] = (m_Sequence++ & 0x0f) << 4;
    report[2] = 0x90;
    report[3] = 63;
    report[4] = 0xfd;
    report[5] = 0xf7;
    report[8] = 0x7f;
    report[9] = 0x7f;
    report[10] = 0xff;
    report[11] = 0x09;
    report[13] = 0x0f;
    report[39] = 0x0a;
    report[40] = 0x07;
    report[43] = 0x02;
    report[44] = 0x01;
    report[46] = 0xff;
    report[47] = 0xd7;
    const uint32_t crc = bluetoothCrc(report.data(), report.size() - 4);
    for (int i = 0; i < 4; i++) {
        report[report.size() - 4 + i] = (crc >> (i * 8)) & 0xff;
    }
    m_WakeSent = sendRawReportLocked(report.data(), report.size());
    return m_WakeSent;
}

bool DualSenseBluetoothRenderer::sendStateLocked()
{
    std::array<uint8_t, 78> report = {};
    report[0] = 0x31;
    report[1] = (m_Sequence++ & 0x0f) << 4;
    report[2] = 0x10;
    const bool compatibleRumble = m_LeftRumble != 0 || m_RightRumble != 0;
    report[3] = compatibleRumble ? 0xff : 0xfc;
    report[4] = 0xd5;
    report[5] = m_RightRumble;
    report[6] = m_LeftRumble;
    report[8] = 0x64;
    report[9] = 0xff;
    report[10] = 0x09;
    std::copy(m_RightTrigger.begin(), m_RightTrigger.end(), report.begin() + 13);
    std::copy(m_LeftTrigger.begin(), m_LeftTrigger.end(), report.begin() + 24);
    report[39] = 0x0a;
    report[40] = 0x03;
    report[41] = 0x03;
    report[44] = 0x02;
    report[46] = (m_PlayerLeds & 0x1f) | 0x20;
    report[47] = m_Red;
    report[48] = m_Green;
    report[49] = m_Blue;
    const uint32_t crc = bluetoothCrc(report.data(), report.size() - 4);
    for (int i = 0; i < 4; i++) {
        report[report.size() - 4 + i] = (crc >> (i * 8)) & 0xff;
    }
    return sendRawReportLocked(report.data(), report.size());
}

bool DualSenseBluetoothRenderer::sendMediaLocked()
{
    if (!m_WakeSent && !sendWakeLocked()) {
        return false;
    }

    std::array<uint8_t, 398> report = {};
    report[0] = 0x36;
    report[1] = (m_Sequence++ & 0x0f) << 4;
    report[2] = 0x91;
    report[3] = 7;
    report[4] = 0xfe;
    std::fill(report.begin() + 5, report.begin() + 10, 96);
    report[10] = m_PacketCounter++;

    report[11] = 0x90;
    report[12] = 63;
    const bool compatibleRumble = m_LeftRumble != 0 || m_RightRumble != 0;
    report[13] = compatibleRumble ? 0xff : 0xfc;
    report[14] = 0xd5;
    report[15] = m_RightRumble;
    report[16] = m_LeftRumble;
    report[18] = 0x64;
    report[19] = 0xff;
    report[20] = 0x09;
    std::copy(m_RightTrigger.begin(), m_RightTrigger.end(), report.begin() + 23);
    std::copy(m_LeftTrigger.begin(), m_LeftTrigger.end(), report.begin() + 34);
    report[49] = 0x0a;
    report[50] = 0x03;
    report[51] = 0x03;
    report[54] = 0x02;
    report[56] = (m_PlayerLeds & 0x1f) | 0x20;
    report[57] = m_Red;
    report[58] = m_Green;
    report[59] = m_Blue;

    report[76] = 0x92;
    report[77] = static_cast<uint8_t>(m_HapticBlock.size());
    std::memcpy(report.data() + 78, m_HapticBlock.data(), m_HapticBlock.size());
    report[142] = 0x93;
    report[143] = 200;

    const uint32_t crc = bluetoothCrc(report.data(), report.size() - 4);
    for (int i = 0; i < 4; i++) {
        report[report.size() - 4 + i] = (crc >> (i * 8)) & 0xff;
    }
    return sendRawReportLocked(report.data(), report.size());
}

void DualSenseBluetoothRenderer::resetStreamLocked()
{
    m_LeftLane.fill(0.0);
    m_RightLane.fill(0.0);
    m_HapticBlock.fill(0);
    m_RingPosition = 0;
    m_DecimationPhase = 0;
    m_HapticPosition = 0;
    m_ExpectedSequence = -1;
}

int8_t DualSenseBluetoothRenderer::filterLaneLocked(const std::array<double, 127>& lane) const
{
    double value = 0.0;
    int index = m_RingPosition == 0 ? 126 : m_RingPosition - 1;
    for (std::size_t tap = 0; tap < m_Filter.size(); tap++) {
        value += lane[index] * m_Filter[tap];
        if (--index < 0) {
            index = 126;
        }
    }
    return static_cast<int8_t>(
        std::clamp(static_cast<int>(std::lround(value * 127.0 / 32768.0)), -128, 127));
}

void DualSenseBluetoothRenderer::setController(int controllerNumber, SDL_GameController* controller,
                                               bool bluetooth)
{
    std::lock_guard lock(m_Mutex);
    if (m_Controller == controller && m_ControllerNumber == controllerNumber &&
        m_Active == (controller != nullptr && bluetooth)) {
        return;
    }

    m_Controller = controller;
    m_ControllerNumber = controllerNumber;
    m_Active = controller != nullptr && bluetooth;
    m_WakeSent = false;
    m_Sequence = 0;
    m_PacketCounter = 0;
    resetStreamLocked();

    if (m_Active) {
        m_Active = sendWakeLocked() && sendStateLocked();
        SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO, "DualSense Bluetooth media transport: %s",
                    m_Active ? "ready" : "unavailable (compatible SDL runtime required)");
    }
}

bool DualSenseBluetoothRenderer::isActive() const
{
    std::lock_guard lock(m_Mutex);
    return m_Active;
}

void DualSenseBluetoothRenderer::submit(const LI_DS5_HAPTICS_PCM_FRAME& frame)
{
    std::lock_guard lock(m_Mutex);
    if (!m_Active || !matchesController(m_ControllerNumber, frame.controllerNumber)) {
        return;
    }

    const bool discontinuity = (frame.flags & (LI_DS5_HAPTICS_PCM_FLAG_STREAM_START |
                                               LI_DS5_HAPTICS_PCM_FLAG_DISCONTINUITY)) != 0 ||
                               (m_ExpectedSequence >= 0 &&
                                frame.sequenceNumber != static_cast<uint32_t>(m_ExpectedSequence));
    if (discontinuity) {
        resetStreamLocked();
    }
    m_ExpectedSequence = static_cast<int64_t>(frame.sequenceNumber) + 1;

    if ((frame.flags & LI_DS5_HAPTICS_PCM_FLAG_STREAM_END) != 0 && frame.frameCount == 0) {
        m_HapticBlock.fill(0);
        sendMediaLocked();
        resetStreamLocked();
        return;
    }

    if (frame.sampleRate != SampleRate || frame.channelCount != 2 || frame.bitsPerSample != 16 ||
        frame.frameCount == 0 || frame.pcmData == nullptr ||
        frame.pcmDataLength != static_cast<uint32_t>(frame.frameCount) * BytesPerFrame) {
        return;
    }

    auto readS16 = [](const uint8_t* bytes) {
        return static_cast<int16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
    };
    for (uint16_t sample = 0; sample < frame.frameCount; sample++) {
        const uint8_t* source = frame.pcmData + sample * BytesPerFrame;
        m_LeftLane[m_RingPosition] = readS16(source);
        m_RightLane[m_RingPosition] = readS16(source + 2);
        m_RingPosition = (m_RingPosition + 1) % static_cast<int>(m_LeftLane.size());

        if (++m_DecimationPhase == 16) {
            m_DecimationPhase = 0;
            if (m_HapticPosition + 1 < static_cast<int>(m_HapticBlock.size())) {
                m_HapticBlock[m_HapticPosition++] = filterLaneLocked(m_LeftLane);
                m_HapticBlock[m_HapticPosition++] = filterLaneLocked(m_RightLane);
            }
        }

        if (m_HapticPosition == static_cast<int>(m_HapticBlock.size())) {
            if (!sendMediaLocked()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "DualSense Bluetooth haptics report failed: %s",
                            SDL_GetError());
                m_Active = false;
                return;
            }
            m_HapticPosition = 0;
            m_HapticBlock.fill(0);
        }
    }
}

void DualSenseBluetoothRenderer::setRumble(uint16_t controllerNumber, uint16_t lowFrequency,
                                           uint16_t highFrequency)
{
    std::lock_guard lock(m_Mutex);
    if (!m_Active || !matchesController(m_ControllerNumber, controllerNumber)) {
        return;
    }
    m_LeftRumble = static_cast<uint8_t>(std::min(255U, (lowFrequency >> 8) * 2U));
    m_RightRumble = static_cast<uint8_t>(std::min(255U, (highFrequency >> 8) * 2U));
    sendStateLocked();
}

void DualSenseBluetoothRenderer::setLed(uint16_t controllerNumber, uint8_t red, uint8_t green,
                                        uint8_t blue)
{
    std::lock_guard lock(m_Mutex);
    if (!m_Active || !matchesController(m_ControllerNumber, controllerNumber)) {
        return;
    }
    m_Red = red;
    m_Green = green;
    m_Blue = blue;
    sendStateLocked();
}

void DualSenseBluetoothRenderer::setAdaptiveTriggers(uint16_t controllerNumber,
                                                     const DualSenseOutputReport* report)
{
    std::lock_guard lock(m_Mutex);
    if (!m_Active || !matchesController(m_ControllerNumber, controllerNumber) ||
        report == nullptr) {
        return;
    }
    if (report->validFlag0 & DS_EFFECT_RIGHT_TRIGGER) {
        m_RightTrigger[0] = report->rightTriggerEffectType;
        std::copy(report->rightTriggerEffect, report->rightTriggerEffect + DS_EFFECT_PAYLOAD_SIZE,
                  m_RightTrigger.begin() + 1);
    }
    if (report->validFlag0 & DS_EFFECT_LEFT_TRIGGER) {
        m_LeftTrigger[0] = report->leftTriggerEffectType;
        std::copy(report->leftTriggerEffect, report->leftTriggerEffect + DS_EFFECT_PAYLOAD_SIZE,
                  m_LeftTrigger.begin() + 1);
    }
    if (report->validFlag2 & 0x01) {
        m_PlayerLeds = report->playerLeds & 0x1f;
    }
    sendStateLocked();
}

void DualSenseBluetoothRenderer::reset()
{
    std::lock_guard lock(m_Mutex);
    resetStreamLocked();
    if (m_Active) {
        m_HapticBlock.fill(0);
        sendMediaLocked();
    }
}
