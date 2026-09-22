# DualSense Bluetooth on Windows

This Windows-only integration connects Foundation Sunshine's native DualSense
haptics stream to a DualSense paired directly with the Moonlight client over
Bluetooth. It does not require USB or a dedicated controller dongle.

## Implemented output

- Foundation packet `0x550A` (48 kHz stereo actuator PCM) is filtered and
  converted into native DualSense Bluetooth media reports.
- Adaptive-trigger state, lightbar state, player LEDs, and compatible rumble
  are retained when haptic media reports are sent.
- Stream starts, sequence gaps, discontinuities, and stream ends reset the
  Bluetooth haptic pipeline so stale feedback is not played later.
- USB haptics remain available through the existing Windows four-channel audio
  endpoint and are used when the selected DualSense is wired.

The Windows dependency setup builds a pinned SDL3 plus `sdl2-compat` runtime
using the existing DualSense Bluetooth raw-report implementation from
`Taveszfito/SDL`. Set `MOONLIGHT_SKIP_DUALSENSE_SDL=1` only when intentionally
building without Bluetooth media-report support.

## Controller audio boundary

DualSense Bluetooth media reports can carry Opus audio for the built-in speaker
or the 3.5 mm headset. That transport is not the same stream as the two actuator
channels currently sent by Foundation Sunshine.

This milestone therefore provides native Bluetooth haptics and adaptive
triggers, but does not claim game-authored controller-speaker audio. Adding it
correctly requires Foundation Sunshine to forward the virtual DualSense audio
endpoint's program channels in addition to its actuator channels. Mirroring the
ordinary Moonlight stereo mix to the controller is possible, but is not a
replacement for the game's separate controller-speaker effects.

The controller microphone is a third, independent uplink and is also outside
this milestone.

## Initial test order

1. Pair one DualSense in Windows Bluetooth settings before starting Moonlight.
2. In Moonlight, select **Physical DualSense (native HD haptics)**.
3. Connect to a current Foundation Sunshine host with its virtual DualSense and
   audio-haptics support enabled.
4. Confirm the log contains `DualSense Bluetooth media transport: ready`.
5. Test touchpad and motion, adaptive triggers, and then authored haptics in a
   title with native DualSense support.
6. Disconnect and reconnect the controller and confirm the transport recovers
   without delayed haptic playback.

The implementation is source-verified but still requires validation with a
physical Bluetooth DualSense and a Windows build produced with the bundled SDL
runtime.
