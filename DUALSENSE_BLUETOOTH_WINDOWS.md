# DualSense Bluetooth and controller audio on Windows

This Windows-only integration connects Foundation Sunshine's native DualSense
haptics stream to a DualSense paired directly with the Moonlight client over
Bluetooth. It does not require USB or a dedicated controller dongle.

## Implemented output

- Foundation packet `0x550D` carries the virtual DualSense's native 48 kHz
  four-channel stream. Speaker channels 1/2 are encoded as 160 kbit/s, 10 ms
  stereo Opus, while actuator channels 3/4 are filtered and converted into
  native DualSense Bluetooth media reports.
- Older Foundation hosts remain compatible through the haptics-only `0x550A`
  fallback.
- Adaptive-trigger state, lightbar state, player LEDs, and compatible rumble
  are retained when haptic media reports are sent.
- Stream starts, sequence gaps, discontinuities, and stream ends reset the
  Bluetooth haptic pipeline so stale feedback is not played later.
- Wired DualSense output uses the Windows four-channel controller endpoint for
  both game-authored speaker audio and native haptics, including the required
  controller audio-route activation sequence.

The Windows dependency setup builds a pinned SDL3 plus `sdl2-compat` runtime
using the existing DualSense Bluetooth raw-report implementation from
`Taveszfito/SDL`. Set `MOONLIGHT_SKIP_DUALSENSE_SDL=1` only when intentionally
building without Bluetooth media-report support.

## Controller audio boundary

This milestone carries game-authored controller-speaker audio from the host's
virtual DualSense endpoint. It does not mirror the ordinary Moonlight stereo
mix, and it does not route that mix to a headset connected to the controller's
3.5 mm jack. Moonlight Extended implements that as a separate jack-detection
and main-stream routing feature.

The controller microphone is a third, independent uplink and is also outside
this milestone.

## Initial test order

1. Pair one DualSense in Windows Bluetooth settings before starting Moonlight.
2. In Moonlight, select **Physical DualSense (native HD haptics)** and opt into
   **Play controller-speaker audio** if speaker effects are wanted.
3. Connect to a current Foundation Sunshine host with its virtual DualSense and
   audio-haptics support enabled.
4. Confirm the log contains `DualSense Bluetooth media transport: ready`.
5. Test touchpad and motion, adaptive triggers, controller-speaker effects, and
   then authored haptics in a title with native DualSense support.
6. Disconnect and reconnect the controller and confirm the transport recovers
   without delayed haptic playback.

The implementation is source-verified but still requires validation with a
physical Bluetooth DualSense and a Windows build produced with the bundled SDL
runtime.

## Preferences and upgrades

Fresh profiles use analyzed vibration. Native physical output and speaker audio
are opt-in. Existing profiles with an explicitly saved physical mode retain that
mode and speaker behavior; an explicitly saved speaker toggle takes precedence.
The speaker toggle applies on the next stream: off removes the quad-audio
capability and keeps the existing haptics-only callback. Compatible Foundation
hosts then send `0x550A` instead of `0x550D`. Ordinary audio and the microphone
preference are independent.

**Log video cadence** under Diagnostics is off by default. When enabled it logs
the first frame and approximately five-second windows of frame rates, dropped
frames, host/decode/render latency and RTT. It does not require the overlay.
Changes apply to the next stream. Local logs are diagnostic files, not source
artifacts to publish.

The custom application namespace imports the previous Moonlight namespace only
when the destination profile has no keys. Existing profiles are never overwritten.
Portable mode uses INI namespaces under the executable directory; installed mode
uses the platform's normal QSettings backend. Keep the complete portable folder
when moving between computers, and keep pairing material private.

## Unchanged protocol contract

SDP `ML_FF_DS5_AUDIO_PCM` is `0x20`. Foundation sends encrypted unreliable control
packet `0x550D`. Its payload is version 1 with a minimum 28-byte header:

| Offset | Field | Encoding |
| --- | --- | --- |
| 0 | version | u8, 1 |
| 1 | flags | u8: START=1, END=2, DISCONTINUITY=4 |
| 2 | header size | u16 LE, at least 28 |
| 4 | controller number | u16 LE |
| 6 | frame count | u16 LE, at most 240 |
| 8 | sequence number | u32 LE |
| 12 | presentation time | u64 LE, microseconds |
| 20 | sample rate | u32 LE, 48000 |
| 24 | channel count | u8, 4 |
| 25 | bits per sample | u8, 16 |
| 26 | reserved | u16 LE, zero |

Payload length must equal `headerSize + frameCount * 8`. PCM is interleaved signed
16-bit little-endian speaker left/right followed by actuator left/right. Unknown
flags, versions, reserved values and invalid lengths/formats are rejected.
Hosts suppress duplicate `0x550A` output when quad audio is negotiated. No host
clock, packet number, PCM format or Bluetooth packet cadence changed in this polish.

## Validation limits and release gates

Protocol golden tests cover valid frames, an END marker, frame bounds and
malformed payload rejection. The callback test verifies that missing speaker
callbacks remain null through setup, so SDP cannot accidentally advertise audio.
Settings tests cover migration, pairing-key preservation, explicit preferences,
and speaker defaults. These tests do not establish physical controller behavior.

The native-renderer test uses real Opus encoding/decoding and mocked HID writes.
It verifies 512-input-frame cadence, valid padded 480-frame silence, partial-block
discard on sequence gaps, END/reset silence, neutral rumble and trigger effects
after controller replacement, and transient failure recovery without rebinding.
Retries start at 50 ms and back off to at most 800 ms; stale PCM is discarded.
A failed Bluetooth write retains its selected target and cannot route PCM to USB.
Output readiness is separate from that reservation: initial or later HID failures
clear readiness, so unavailable Bluetooth output does not suppress the launch
warning. A successful state/media retry restores readiness without rebinding.
Media discontinuities preserve independent trigger effects; controller/session
boundaries reset them.

Windows USB routing resolves the selected SDL HID interface's device container
ID and requires exactly one active, matching 48 kHz four-channel audio endpoint.
Friendly names are used only for logging. Missing identity or ambiguous endpoints
fail closed. Queue generations discard old-controller PCM when targets change;
audio-buffer waits are bounded. Tests cover renamed, foreign and ambiguous
endpoint selection and verify that an unidentified USB target receives no output.
Real endpoint identity, reconnects and output still require hardware validation.

The preserved 512-to-480 conversion is documented in the pinned
[upstream Bluetooth packetizer](https://github.com/Taveszfito/moonlight-extended/blob/aee77e490f85e78b6d5bde68f3c3309fdd5063cc/app/streaming/audio/dualsenseaudio.cpp):
one 480-frame Opus block is consumed per 512 source frames by the wireless media
clock. This is primary source evidence, not an independent clock measurement.
Do not change the host's 48 kHz clock to compensate. Physical timing, multi-device
USB routing, reconnects, and end-to-end host/client sessions remain release gates.
This branch is a review candidate, not a hardware-certified replacement.

The native-audio common-library commit and its optional-callback fix must be
published to a reachable dependency fork before public clone/build reproduction.
Offline recovery bundles retain the local source until publication is available.
