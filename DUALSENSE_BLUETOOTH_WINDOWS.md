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
2. In Moonlight, select **Physical DualSense (native HD haptics)**.
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
