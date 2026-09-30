#include "dualsensehaptics.h"
#include "dualsensebluetooth.h"
#include "dualsensehapticsstream.h"
#include "dualsensehapticsrouting.h"
#include "dualsenseusbidentity.h"
#include "streaming/input/dualsenseoutputreport.h"

#include "SDL_compat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <QtGlobal>

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <QString>

using Microsoft::WRL::ComPtr;
#elif defined(Q_OS_MACOS)
#include "dualsensehapticsmac.h"
#endif

namespace {
constexpr std::size_t MaxQueuedPackets = 32;
constexpr std::uint32_t PrebufferFrames = 720; // 15 ms at 48 kHz

struct Packet
{
    std::uint8_t flags = 0;
    std::uint16_t controllerNumber = 0;
    std::uint16_t frameCount = 0;
    std::uint8_t channelCount = 2;
    std::uint32_t sequenceNumber = 0;
    std::uint64_t targetGeneration = 0;
    std::vector<std::uint8_t> pcm;
};
}

struct DualSenseHapticsRenderer::Impl
{
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Packet> queue;
    std::atomic_bool stopping{false};

#ifdef Q_OS_WIN32
    DualSenseBluetoothRenderer bluetoothRenderer;
    ComPtr<IAudioClient> audioClient;
    ComPtr<IAudioRenderClient> renderClient;
    UINT32 bufferFrames = 0;
    WORD bitsPerSample = 0;
    bool floatSamples = false;
    bool streamStarted = false;
    dualsense_haptics::PcmStreamTracker streamTracker;
    std::deque<Packet> prebuffer;
    std::uint32_t prebufferedFrames = 0;
    std::chrono::steady_clock::time_point nextEndpointProbe{};
    SDL_GameController* usbController = nullptr;
    int usbControllerNumber = -1;
    GUID usbContainer = GUID_NULL;
    GUID endpointContainer = GUID_NULL; // Accessed only by the COM worker.
    bool usbTargetReady = false;
    bool targetChanged = false;
    std::atomic_uint64_t targetGeneration{0};

    // Keep this last: run() may access every member as soon as the thread starts.
    std::thread worker;

    Impl()
    {
        worker = std::thread([this] { run(); });
    }

    ~Impl()
    {
        stopping = true;
        condition.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    static bool matchesContainer(IMMDevice* device, const GUID& container)
    {
        if (container == GUID_NULL) return false;
        ComPtr<IPropertyStore> properties;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return false;
        PROPVARIANT value;
        PropVariantInit(&value);
        const bool matches = SUCCEEDED(properties->GetValue(PKEY_Device_ContainerId, &value)) &&
                             value.vt == VT_CLSID && value.puuid != nullptr &&
                             *value.puuid == container;
        PropVariantClear(&value);
        return matches;
    }
    static bool classifyFormat(const WAVEFORMATEX* format, bool& isFloat, WORD& bits)
    {
        if (format->nSamplesPerSec != 48000 || format->nChannels != 4) {
            return false;
        }

        GUID subtype = GUID_NULL;
        if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            subtype = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat;
        }
        else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            subtype = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        }
        else if (format->wFormatTag == WAVE_FORMAT_PCM) {
            subtype = KSDATAFORMAT_SUBTYPE_PCM;
        }

        bits = format->wBitsPerSample;
        isFloat = subtype == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && bits == 32;
        return isFloat || (subtype == KSDATAFORMAT_SUBTYPE_PCM && (bits == 16 || bits == 32));
    }

    static bool findEndpoint(const GUID& container, ComPtr<IAudioClient>& candidate,
                             std::vector<BYTE>& mixBytes, std::wstring& friendlyName)
    {
        if (container == GUID_NULL) return false;
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDeviceCollection> devices;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator))) ||
            FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return false;
        UINT count = 0;
        if (FAILED(devices->GetCount(&count))) return false;
        const int selected = dualsense_haptics::selectUniqueMatchingEndpoint(count, [&](std::size_t index) {
            ComPtr<IMMDevice> device;
            if (FAILED(devices->Item(static_cast<UINT>(index), &device)) ||
                !matchesContainer(device.Get(), container)) return false;
            ComPtr<IAudioClient> audio;
            if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(audio.GetAddressOf())))) return false;
            WAVEFORMATEX* mix = nullptr;
            if (FAILED(audio->GetMixFormat(&mix)) || mix == nullptr) {
                CoTaskMemFree(mix);
                return false;
            }
            bool isFloat = false;
            WORD bits = 0;
            const bool supported = classifyFormat(mix, isFloat, bits);
            if (supported) {
                const auto* bytes = reinterpret_cast<const BYTE*>(mix);
                mixBytes.assign(bytes, bytes + sizeof(WAVEFORMATEX) + mix->cbSize);
                candidate = audio;
                friendlyName.clear();
                ComPtr<IPropertyStore> properties;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                    PROPVARIANT name;
                    PropVariantInit(&name);
                    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
                        name.vt == VT_LPWSTR && name.pwszVal) friendlyName = name.pwszVal;
                    PropVariantClear(&name);
                }
            }
            CoTaskMemFree(mix);
            return supported;
        });
        // Zero matches and multiple matches both fail closed. Never fall back to
        // a friendly-name match or the first four-channel device on the machine.
        return selected >= 0;
    }

    bool openEndpoint()
    {
        ComPtr<IAudioClient> candidate;
        std::vector<BYTE> mixBytes;
        std::wstring friendlyName;
        if (!findEndpoint(endpointContainer, candidate, mixBytes, friendlyName)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                        "Selected USB DualSense has no unique matching four-channel audio endpoint");
            return false;
        }
        auto* mix = reinterpret_cast<WAVEFORMATEX*>(mixBytes.data());
        bool candidateFloat = false;
        WORD candidateBits = 0;
        if (!classifyFormat(mix, candidateFloat, candidateBits) ||
            FAILED(candidate->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST,
                                         500000, 0, mix, nullptr))) return false;
        ComPtr<IAudioRenderClient> candidateRenderer;
        if (FAILED(candidate->GetService(IID_PPV_ARGS(&candidateRenderer))) ||
            FAILED(candidate->GetBufferSize(&bufferFrames))) return false;
        audioClient = candidate;
        renderClient = candidateRenderer;
        floatSamples = candidateFloat;
        bitsPerSample = candidateBits;
        const QByteArray name = QString::fromStdWString(friendlyName).toUtf8();
        SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO,
                    "Selected DualSense USB endpoint ready: %s (48 kHz, 4 ch, %u-bit%s)",
                    name.constData(), bitsPerSample, floatSamples ? " float" : " PCM");
        return true;
    }
    void resetAudioStream()
    {
        if (audioClient) {
            if (streamStarted) audioClient->Stop();
            audioClient->Reset();
        }
        streamStarted = false;
        prebuffer.clear();
        prebufferedFrames = 0;
    }

    void resetStream()
    {
        resetAudioStream();
        streamTracker.reset();
    }

    bool writePacket(const Packet& packet)
    {
        if (!audioClient || !renderClient || packet.frameCount == 0) return true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (!stopping) {
            if (packet.targetGeneration != targetGeneration.load() ||
                std::chrono::steady_clock::now() >= deadline) return false;
            UINT32 padding = 0;
            if (FAILED(audioClient->GetCurrentPadding(&padding))) return false;
            if (bufferFrames - padding >= packet.frameCount) break;
            Sleep(1);
        }
        if (stopping) return false;

        BYTE* output = nullptr;
        if (FAILED(renderClient->GetBuffer(packet.frameCount, &output))) return false;
        const auto* input = reinterpret_cast<const std::int16_t*>(packet.pcm.data());
        if (floatSamples) {
            auto* samples = reinterpret_cast<float*>(output);
            for (std::uint16_t i = 0; i < packet.frameCount; i++) {
                samples[i * 4] = packet.channelCount == 4 ? input[i * 4] / 32768.0f : 0.0f;
                samples[i * 4 + 1] = packet.channelCount == 4 ? input[i * 4 + 1] / 32768.0f : 0.0f;
                const auto hapticOffset = packet.channelCount == 4 ? i * 4 + 2 : i * 2;
                samples[i * 4 + 2] = input[hapticOffset] / 32768.0f;
                samples[i * 4 + 3] = input[hapticOffset + 1] / 32768.0f;
            }
        }
        else if (bitsPerSample == 16) {
            auto* samples = reinterpret_cast<std::int16_t*>(output);
            for (std::uint16_t i = 0; i < packet.frameCount; i++) {
                samples[i * 4] = packet.channelCount == 4 ? input[i * 4] : 0;
                samples[i * 4 + 1] = packet.channelCount == 4 ? input[i * 4 + 1] : 0;
                const auto hapticOffset = packet.channelCount == 4 ? i * 4 + 2 : i * 2;
                samples[i * 4 + 2] = input[hapticOffset];
                samples[i * 4 + 3] = input[hapticOffset + 1];
            }
        }
        else {
            auto* samples = reinterpret_cast<std::int32_t*>(output);
            for (std::uint16_t i = 0; i < packet.frameCount; i++) {
                samples[i * 4] =
                    packet.channelCount == 4 ? static_cast<std::int32_t>(input[i * 4]) * 65536 : 0;
                samples[i * 4 + 1] = packet.channelCount == 4
                                         ? static_cast<std::int32_t>(input[i * 4 + 1]) * 65536
                                         : 0;
                const auto hapticOffset = packet.channelCount == 4 ? i * 4 + 2 : i * 2;
                samples[i * 4 + 2] = static_cast<std::int32_t>(input[hapticOffset]) * 65536;
                samples[i * 4 + 3] = static_cast<std::int32_t>(input[hapticOffset + 1]) * 65536;
            }
        }
        const bool currentTarget = packet.targetGeneration == targetGeneration.load();
        return SUCCEEDED(renderClient->ReleaseBuffer(packet.frameCount,
                    currentTarget ? 0 : AUDCLNT_BUFFERFLAGS_SILENT)) && currentTarget;
    }

    bool startPrebufferedStream()
    {
        for (const auto& buffered : prebuffer) {
            if (!writePacket(buffered)) return false;
        }
        prebuffer.clear();
        prebufferedFrames = 0;
        if (FAILED(audioClient->Start())) return false;
        streamStarted = true;
        return true;
    }

    void process(Packet packet)
    {
        if (packet.targetGeneration != targetGeneration.load()) return;
        const auto action = streamTracker.observe(packet.flags, packet.controllerNumber,
                                                  packet.sequenceNumber);
        if (action == dualsense_haptics::PcmStreamTracker::Action::Ignore) {
            return;
        }
        if (action == dualsense_haptics::PcmStreamTracker::Action::End) {
            resetAudioStream();
            return;
        }
        if (action == dualsense_haptics::PcmStreamTracker::Action::ResetAndAccept) {
            resetAudioStream();
        }

        if (!audioClient) {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextEndpointProbe) {
                return;
            }
            if (!openEndpoint()) {
                nextEndpointProbe = now + std::chrono::seconds(2);
                return;
            }
        }
        if (!streamStarted) {
            if (packet.frameCount > bufferFrames) {
                SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                             "DualSense haptics packet (%u frames) exceeds the endpoint buffer (%u frames)",
                             packet.frameCount, bufferFrames);
                audioClient.Reset();
                renderClient.Reset();
                resetStream();
                return;
            }

            // A shared-mode endpoint may expose less than our preferred 15 ms
            // jitter buffer. Never queue more than the endpoint can accept before
            // Start(), or writePacket() would wait for a device that is not running.
            if (!prebuffer.empty() && prebufferedFrames + packet.frameCount > bufferFrames) {
                if (!startPrebufferedStream()) {
                    audioClient.Reset();
                    renderClient.Reset();
                    resetStream();
                    return;
                }
                if (!writePacket(packet)) {
                    audioClient.Reset();
                    renderClient.Reset();
                    resetStream();
                }
                return;
            }

            prebufferedFrames += packet.frameCount;
            prebuffer.emplace_back(std::move(packet));
            const auto targetFrames = std::min(PrebufferFrames, bufferFrames);
            if (prebufferedFrames < targetFrames) return;

            if (!startPrebufferedStream()) {
                audioClient.Reset();
                renderClient.Reset();
                resetStream();
                return;
            }
        }
        else if (!writePacket(packet)) {
            audioClient.Reset();
            renderClient.Reset();
            resetStream();
        }
    }

    void run()
    {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult)) {
            SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                         "Unable to initialize COM for DualSense haptics: 0x%08lx",
                         static_cast<unsigned long>(comResult));
            return;
        }

        while (!stopping) {
            Packet packet;
            bool changed = false;
            bool havePacket = false;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || targetChanged || !queue.empty(); });
                if (stopping) break;
                changed = targetChanged;
                if (changed) {
                    endpointContainer = usbContainer;
                    targetChanged = false;
                }
                if (!queue.empty()) {
                    packet = std::move(queue.front());
                    queue.pop_front();
                    havePacket = true;
                }
            }
            if (changed) {
                resetStream();
                renderClient.Reset();
                audioClient.Reset();
                nextEndpointProbe = {};
            }
            if (havePacket) process(std::move(packet));
        }

        resetStream();
        renderClient.Reset();
        audioClient.Reset();
        CoUninitialize();
    }

    void enqueuePacket(Packet packet)
    {
        {
            std::lock_guard lock(mutex);
            if (!dualsense_haptics::canUseNativeController(packet.controllerNumber,
                    usbControllerNumber, usbTargetReady ? 1 : 0)) return;
            packet.targetGeneration = targetGeneration.load();
            if (queue.size() == MaxQueuedPackets) {
                queue.pop_front();
                packet.flags |= LI_DS5_HAPTICS_PCM_FLAG_DISCONTINUITY;
            }
            queue.emplace_back(std::move(packet));
        }
        condition.notify_one();
    }

    void setUsbController(int controllerNumber, SDL_GameController* controller)
    {
        GUID container = GUID_NULL;
        const bool identified = dualSenseUsbContainer(controller, container);
        {
            std::lock_guard lock(mutex);
            if (usbController == controller && usbControllerNumber == controllerNumber &&
                usbContainer == container) return;
            usbController = controller;
            usbControllerNumber = controllerNumber;
            usbContainer = identified ? container : GUID_NULL;
            usbTargetReady = false;
            targetGeneration++;
            queue.clear();
            targetChanged = true;
        }
        condition.notify_one();
        if (!identified) return;

        auto send = [controller](const std::array<uint8_t, sizeof(DualSenseOutputReport)>& effect) {
            return SDL_GameControllerSendEffect(controller, effect.data(),
                                                static_cast<int>(effect.size())) == 0;
        };
        std::array<uint8_t, sizeof(DualSenseOutputReport)> route = {};
        route[0] = 0xf3;
        route[4] = 0xff;
        route[5] = 0xff;
        route[6] = 0x40;
        route[7] = 0x01;
        bool routeOk = send(route);
        SDL_Delay(40);
        routeOk = send(route) && routeOk;

        std::array<uint8_t, sizeof(DualSenseOutputReport)> musicRumble = {};
        musicRumble[0] = 0xe0;
        musicRumble[4] = 0x7f;
        musicRumble[5] = 0xff;
        musicRumble[6] = 0x40;
        musicRumble[7] = 0x30;
        SDL_Delay(35);
        const bool musicOk = send(musicRumble);

        std::array<uint8_t, sizeof(DualSenseOutputReport)> wake = {};
        wake[1] = 0x15;
        wake[38] = 0x03;
        wake[41] = 0x02;
        wake[43] = 0x24;
        SDL_Delay(35);
        const bool wakeOk = send(wake);
        SDL_Delay(35);
        routeOk = send(route) && routeOk;
        {
            std::lock_guard lock(mutex);
            usbTargetReady = routeOk && musicOk && wakeOk;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO,
                    "DualSense USB audio activation: route=%s music-rumble=%s dsp-wake=%s",
                    routeOk ? "ok" : "failed", musicOk ? "ok" : "failed", wakeOk ? "ok" : "failed");
    }
#else
    Impl()
    {
#ifdef Q_OS_MACOS
        macRenderer = std::make_unique<MacDualSenseHapticsRenderer>();
#endif
    }
    ~Impl() = default;

#ifdef Q_OS_MACOS
    std::unique_ptr<MacDualSenseHapticsRenderer> macRenderer;
#endif
#endif
};

DualSenseHapticsRenderer::DualSenseHapticsRenderer() : m_Impl(std::make_unique<Impl>()) {}
DualSenseHapticsRenderer::~DualSenseHapticsRenderer() = default;

bool DualSenseHapticsRenderer::isAvailable(SDL_GameController* controller)
{
#ifdef Q_OS_WIN32
    GUID container;
    if (!dualSenseUsbContainer(controller, container)) return false;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) return false;
    ComPtr<IAudioClient> candidate;
    std::vector<BYTE> mixBytes;
    std::wstring name;
    const bool found = Impl::findEndpoint(container, candidate, mixBytes, name);
    candidate.Reset();
    if (shouldUninitialize) CoUninitialize();
    return found;
#else
    (void)controller;
    return false;
#endif
}
void DualSenseHapticsRenderer::submit(const LI_DS5_HAPTICS_PCM_FRAME& frame)
{
#ifdef Q_OS_WIN32
    if (frame.sampleRate != 48000 || frame.channelCount != 2 || frame.bitsPerSample != 16 ||
        frame.frameCount > 480 || frame.pcmDataLength != frame.frameCount * 4 ||
        (frame.pcmDataLength != 0 && frame.pcmData == nullptr)) {
        return;
    }

    if (m_Impl->bluetoothRenderer.isActive()) {
        m_Impl->bluetoothRenderer.submit(frame);
        return;
    }

    Packet packet;
    packet.flags = frame.flags;
    packet.controllerNumber = frame.controllerNumber;
    packet.frameCount = frame.frameCount;
    packet.channelCount = 2;
    packet.sequenceNumber = frame.sequenceNumber;
    if (frame.pcmDataLength != 0) {
        packet.pcm.assign(frame.pcmData, frame.pcmData + frame.pcmDataLength);
    }
    m_Impl->enqueuePacket(std::move(packet));
#else
    (void)frame;
#endif
}

void DualSenseHapticsRenderer::setControllerTarget(int controllerNumber,
                                                   SDL_GameController* controller, bool bluetooth)
{
#ifdef Q_OS_WIN32
    m_Impl->setUsbController(controllerNumber, bluetooth ? nullptr : controller);
    m_Impl->bluetoothRenderer.setController(controllerNumber, controller, bluetooth);
#elif defined(Q_OS_MACOS)
    if (m_Impl->macRenderer != nullptr) {
        m_Impl->macRenderer->setControllerTarget(controllerNumber);
    }
#else
    (void)controllerNumber;
    (void)controller;
    (void)bluetooth;
#endif
}

void DualSenseHapticsRenderer::submit(const LI_DS5_AUDIO_PCM_FRAME& frame)
{
#ifdef Q_OS_WIN32
    if (frame.sampleRate != 48000 || frame.channelCount != 4 || frame.bitsPerSample != 16 ||
        frame.frameCount > 240 || frame.pcmDataLength != frame.frameCount * 8 ||
        (frame.pcmDataLength != 0 && frame.pcmData == nullptr)) {
        return;
    }

    if (m_Impl->bluetoothRenderer.isActive()) {
        m_Impl->bluetoothRenderer.submit(frame);
        return;
    }

    Packet packet;
    packet.flags = frame.flags;
    packet.controllerNumber = frame.controllerNumber;
    packet.frameCount = frame.frameCount;
    packet.channelCount = 4;
    packet.sequenceNumber = frame.sequenceNumber;
    if (frame.pcmDataLength != 0) {
        packet.pcm.assign(frame.pcmData, frame.pcmData + frame.pcmDataLength);
    }
    m_Impl->enqueuePacket(std::move(packet));
#else
    (void)frame;
#endif
}

bool DualSenseHapticsRenderer::isBluetoothActive() const
{
#ifdef Q_OS_WIN32
    return m_Impl->bluetoothRenderer.isActive();
#else
    return false;
#endif
}

bool DualSenseHapticsRenderer::isUsbActive() const
{
#ifdef Q_OS_WIN32
    std::lock_guard lock(m_Impl->mutex);
    return m_Impl->usbTargetReady;
#else
    return false;
#endif
}

void DualSenseHapticsRenderer::setBluetoothRumble(uint16_t controllerNumber, uint16_t lowFrequency,
                                                  uint16_t highFrequency)
{
#ifdef Q_OS_WIN32
    m_Impl->bluetoothRenderer.setRumble(controllerNumber, lowFrequency, highFrequency);
#else
    (void)controllerNumber;
    (void)lowFrequency;
    (void)highFrequency;
#endif
}

void DualSenseHapticsRenderer::setBluetoothLed(uint16_t controllerNumber, uint8_t red,
                                               uint8_t green, uint8_t blue)
{
#ifdef Q_OS_WIN32
    m_Impl->bluetoothRenderer.setLed(controllerNumber, red, green, blue);
#else
    (void)controllerNumber;
    (void)red;
    (void)green;
    (void)blue;
#endif
}

void DualSenseHapticsRenderer::setBluetoothAdaptiveTriggers(uint16_t controllerNumber,
                                                            const DualSenseOutputReport* report)
{
#ifdef Q_OS_WIN32
    m_Impl->bluetoothRenderer.setAdaptiveTriggers(controllerNumber, report);
#else
    (void)controllerNumber;
    (void)report;
#endif
}

void DualSenseHapticsRenderer::reset()
{
#ifdef Q_OS_WIN32
    m_Impl->bluetoothRenderer.reset();
    {
        std::lock_guard lock(m_Impl->mutex);
        m_Impl->targetGeneration++;
        m_Impl->queue.clear();
        m_Impl->targetChanged = true;
    }
    m_Impl->condition.notify_one();
#elif defined(Q_OS_MACOS)
    if (m_Impl->macRenderer != nullptr) {
        m_Impl->macRenderer->reset();
    }
#endif
}

bool DualSenseHapticsRenderer::submit(const LI_DS5_HAPTICS_IR_FRAME_V2& frame,
                                      bool& startedNative)
{
#ifdef Q_OS_MACOS
    return m_Impl->macRenderer != nullptr &&
           m_Impl->macRenderer->submit(frame, startedNative);
#else
    (void)frame;
    startedNative = false;
    return false;
#endif
}
