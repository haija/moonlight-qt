#include <QCoreApplication>
#include "streaming/audio/dualsensebluetooth.h"
#include "streaming/input/dualsenseoutputreport.h"
#include "streaming/audio/dualsensehaptics.h"
#include "streaming/audio/dualsensehapticsrouting.h"
#include <opus.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <vector>

struct Write { SDL_GameController* target; std::vector<uint8_t> report; bool failed; };
static std::vector<Write> writes;
static uint64_t nowMs = 1000;
static bool failMediaOnce, failAll;
extern "C" {
Uint64 SDLCALL SDL_GetTicks64(void) { return nowMs; }
SDL_bool SDLCALL SDL_GameControllerGetAttached(SDL_GameController*) { return SDL_TRUE; }
const char* SDLCALL SDL_GetError(void) { return "injected HID failure"; }
void SDLCALL SDL_LogWarn(int, const char*, ...) {}
void SDLCALL SDL_LogInfo(int, const char*, ...) {}
void SDLCALL SDL_LogError(int, const char*, ...) {}
void SDLCALL SDL_Delay(Uint32) {}
SDL_Joystick* SDLCALL SDL_GameControllerGetJoystick(SDL_GameController* controller)
{ return reinterpret_cast<SDL_Joystick*>(controller); }
const char* SDLCALL SDL_JoystickPath(SDL_Joystick*) { return "invalid-test-hid-interface"; }
int SDLCALL SDL_GameControllerSendEffect(SDL_GameController* target, const void* bytes, int size)
{
    const auto* data = static_cast<const uint8_t*>(bytes);
    if (size < 9 || std::memcmp(data, "ARTBT01", 7) != 0) return -1;
    const bool failure = failAll || (failMediaOnce && data[8] == 0x36);
    if (data[8] == 0x36 && failMediaOnce) failMediaOnce = false;
    writes.push_back({target, {data + 8, data + size}, failure});
    return failure ? -1 : 0;
}
}

static void require(bool condition, const char* message)
{
    if (!condition) qFatal("%s", message);
}
static const Write& lastMedia()
{
    for (auto it = writes.rbegin(); it != writes.rend(); ++it)
        if (it->report[0] == 0x36 && !it->failed) return *it;
    qFatal("No successful media report");
    std::abort();
}
static void requireSilent(const Write& write)
{
    require(write.report.size() == 398 && write.report[142] == 0x93 &&
            write.report[143] == 200, "Speaker report framing changed");
    int error = 0;
    OpusDecoder* decoder = opus_decoder_create(48000, 2, &error);
    require(decoder && error == OPUS_OK, "Opus decoder unavailable");
    std::array<int16_t, 480 * 2> output = {};
    require(opus_decode(decoder, write.report.data() + 144, 200, output.data(), 480, 0) == 480,
            "Silence payload is not valid 10 ms stereo Opus");
    require(std::all_of(output.begin(), output.end(), [](int16_t s) { return s >= -1 && s <= 1; }),
            "Stream boundary replayed speaker audio");
    opus_decoder_destroy(decoder);
}
static void quad(DualSenseBluetoothRenderer& renderer, uint32_t& sequence,
                 unsigned frames, int16_t speaker = 0, bool start = false, uint16_t controller = 3)
{
    while (frames) {
        const unsigned count = std::min(frames, 128U);
        std::array<int16_t, 128 * 4> pcm = {};
        for (unsigned i = 0; i < count; i++) pcm[i * 4] = pcm[i * 4 + 1] = speaker;
        LI_DS5_AUDIO_PCM_FRAME frame = {};
        frame.flags = start ? LI_DS5_AUDIO_PCM_FLAG_STREAM_START : 0;
        frame.controllerNumber = controller;
        frame.sequenceNumber = sequence++;
        frame.frameCount = static_cast<uint16_t>(count);
        frame.sampleRate = 48000; frame.channelCount = 4; frame.bitsPerSample = 16;
        frame.pcmData = reinterpret_cast<const uint8_t*>(pcm.data()); frame.pcmDataLength = count * 8;
        renderer.submit(frame);
        frames -= count; start = false;
    }
}
static void haptics(DualSenseBluetoothRenderer& renderer, uint32_t& sequence)
{
    const std::array<uint8_t, 256 * 4> pcm = {};
    LI_DS5_HAPTICS_PCM_FRAME frame = {};
    frame.controllerNumber = 3; frame.frameCount = 256; frame.sampleRate = 48000;
    frame.channelCount = 2; frame.bitsPerSample = 16; frame.pcmData = pcm.data();
    frame.pcmDataLength = static_cast<uint32_t>(pcm.size());
    for (unsigned i = 0; i < 2; i++) { frame.sequenceNumber = sequence++; renderer.submit(frame); }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const struct { int container; bool fourChannel; const char* name; } endpoints[] = {
        {12, true, "Wireless Controller"}, {42, true, "Renamed device"},
        {42, false, "DualSense"}, {42, true, "Duplicate endpoint"}
    };
    const auto matches = [&](std::size_t index) {
        return endpoints[index].container == 42 && endpoints[index].fourChannel;
    };
    require(dualsense_haptics::selectUniqueMatchingEndpoint(3, matches) == 1,
            "Routing selected a friendly-name match or ignored a renamed device");
    require(dualsense_haptics::selectUniqueMatchingEndpoint(4, matches) == -1,
            "Ambiguous USB endpoint selection did not fail closed");
    require(dualsense_haptics::selectUniqueMatchingEndpoint(1, matches) == -1,
            "Routing accepted a different controller container");
    auto* first = reinterpret_cast<SDL_GameController*>(uintptr_t(1));
    auto* second = reinterpret_cast<SDL_GameController*>(uintptr_t(2));
#ifdef Q_OS_WIN32
    {
        DualSenseHapticsRenderer renderer;
        const auto count = writes.size();
        renderer.setControllerTarget(3, first, false);
        LI_DS5_AUDIO_PCM_FRAME packet = {};
        packet.controllerNumber = 3; packet.sampleRate = 48000;
        packet.channelCount = 4; packet.bitsPerSample = 16;
        renderer.submit(packet);
        require(writes.size() == count, "Unidentified USB target received an activation report");
    }
#endif
    {
        DualSenseBluetoothRenderer renderer;
        renderer.setController(3, first, true);
        uint32_t sequence = 0;
        quad(renderer, sequence, 511, 0, true);
        require(std::none_of(writes.begin(), writes.end(), [](const Write& w) { return w.report[0] == 0x36; }),
                "Wireless media cadence changed before 512 input frames");
        quad(renderer, sequence, 1);
        requireSilent(lastMedia());
        const auto count = writes.size();
        quad(renderer, sequence, 512, 0, false, 4);
        require(writes.size() == count, "Wrong logical controller accepted");
        renderer.setRumble(3, 65535, 65535);
        DualSenseOutputReport effects = {};
        effects.validFlag0 = DS_EFFECT_RIGHT_TRIGGER | DS_EFFECT_LEFT_TRIGGER;
        effects.rightTriggerEffectType = effects.leftTriggerEffectType = 0x21;
        effects.rightTriggerEffect[0] = effects.leftTriggerEffect[0] = 0x7f;
        renderer.setAdaptiveTriggers(3, &effects);
        quad(renderer, sequence, 256, 22000);
        sequence += 2; // A gap must discard the partial speaker block and reset Opus.
        quad(renderer, sequence, 512);
        requireSilent(lastMedia());
        require(lastMedia().report[23] == 0x21 && lastMedia().report[34] == 0x21,
                "Media discontinuity cleared independent trigger effects");
        quad(renderer, sequence, 512, 22000);
        haptics(renderer, sequence); // Continuous sequence when the host disables speaker audio.
        requireSilent(lastMedia());
        LI_DS5_AUDIO_PCM_FRAME end = {};
        end.controllerNumber = 3; end.sequenceNumber = sequence++;
        end.flags = LI_DS5_AUDIO_PCM_FLAG_STREAM_END;
        renderer.submit(end);
        requireSilent(lastMedia());
        haptics(renderer, sequence);
        requireSilent(lastMedia());
        LI_DS5_HAPTICS_PCM_FRAME legacyEnd = {};
        legacyEnd.controllerNumber = 3;
        legacyEnd.sequenceNumber = sequence++;
        legacyEnd.flags = LI_DS5_HAPTICS_PCM_FLAG_STREAM_END;
        renderer.submit(legacyEnd);
        requireSilent(lastMedia());
        renderer.setController(-1, nullptr, false);
        const auto boundary = writes.size();
        renderer.setController(3, second, true);
        require(writes.size() > boundary, "Replacement controller was not initialized");
        for (std::size_t i = boundary; i < writes.size(); i++) {
            const auto& r = writes[i].report;
            if (r[0] == 0x31) {
                require(r[5] == 0 && r[6] == 0 && r[13] == 0x05 && r[24] == 0x05,
                        "Old rumble/trigger effects survived controller replacement");
            }
        }
        renderer.setRumble(3, 65535, 65535);
        renderer.reset();
        require(lastMedia().report[15] == 0 && lastMedia().report[16] == 0,
                "Session reset retained rumble");
        requireSilent(lastMedia());
    }
    writes.clear();
    {
        DualSenseBluetoothRenderer renderer;
        renderer.setController(3, first, true);
        uint32_t sequence = 0;
        failMediaOnce = true;
        haptics(renderer, sequence);
        require(renderer.isActive(), "One send failure latched the Bluetooth target off");
        const auto count = writes.size();
        for (unsigned i = 0; i < 100; i++) haptics(renderer, sequence);
        require(writes.size() == count, "Backoff did not bound HID attempts");
        nowMs += 50;
        haptics(renderer, sequence);
        requireSilent(lastMedia());
    }
    writes.clear(); failAll = true;
    {
        DualSenseBluetoothRenderer renderer;
        renderer.setController(3, first, true);
        uint32_t sequence = 0;
        const auto count = writes.size();
        quad(renderer, sequence, 2048);
        require(writes.size() == count && renderer.isActive(), "Unavailable HID was retried without backoff");
        nowMs += 50;
        quad(renderer, sequence, 512);
        const auto afterRetry = writes.size();
        nowMs += 99;
        quad(renderer, sequence, 2048);
        require(writes.size() == afterRetry, "Exponential retry interval was not enforced");
        failAll = false; nowMs += 1;
        quad(renderer, sequence, 512);
        requireSilent(lastMedia());
    }
    qInfo("DualSense Bluetooth codec, effect-boundary, cadence and retry tests passed");
    return 0;
}
