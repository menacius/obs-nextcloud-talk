# Nextcloud Talk plugin for OBS

Native OBS Studio plugin foundation for using OBS as a Nextcloud Talk call endpoint.

## Current milestone (0.7.0)

Implemented:

- Native **Nextcloud Talk** OBS dock.
- Plugin version displayed in the dock header, with account and outgoing-device settings moved to **Tools → Nextcloud Talk Settings**.
- Chat history, live messages, and message sending for the joined call directly in the dock.
- Nextcloud OCS authentication with a username and app password.
- Conversation listing through Talk API v4.
- Call join, leave, and participant polling through Talk API v4.
- Active Talk conversation-session creation before joining or starting a call.
- Separate **Start call** and **Join call** actions based on the room's real `hasCall` state.
- One visible **Nextcloud Talk** source type in Add Source, with participant camera/audio and screen-share choices.
- Video, audio, and screenshare sources are created through the single **Nextcloud Talk** source type as `V - user - Talk`, `A - user - Talk`, and `S - user - Talk`.
- Participant rows show live mute, video, and screen-share state.
- Participant rows show theme-aware speaking/not-speaking activity from Talk's WebRTC status channel.
- Participant context menus can add video plus audio, video only, audio only, or an active screen share directly to the OBS preview scene.
- Video and screenshare instances are hidden from the OBS audio mixer; only Audio instances are active there.
- Participant source identity based on Talk actor identity, so a reconnect does not replace scene items when the actor ID is stable.
- Offline/online state handling; sources remain in scenes while a participant is temporarily absent.
- Outgoing media can use a Video Capture Device plus Audio Input Capture, a named OBS scene, OBS Preview, or OBS Program.
- The settings dialog includes a live 16:9 preview of the selected outgoing video.
- Preview and Program selections follow OBS scene changes while a call is active.
- Device mode requires one video and one audio capture source; scene modes publish the selected scene's audio mix.
- Incoming Talk audio is excluded from the outgoing device selector to prevent the simplest feedback loop.
- Persistent non-secret settings plus an app password stored in Windows Credential Manager.
- Talk signaling-settings discovery through API v3.
- External/HPB signaling authentication and room membership over a native WinHTTP WebSocket.
- STUN/TURN configuration and WebRTC peer connections through libdatachannel.
- Talk SDP and ICE candidate exchange for both MCU subscriber offers and peer-to-peer offers.
- H.264 and VP8 remote video depacketization and decode into participant video sources.
- Reorder-aware VP8 frame assembly that never submits incomplete RTP frames to the decoder.
- VP8 invisible/reference frames preserve decoder continuity instead of causing false recovery loops.
- Opus remote audio depacketization and decode into participant audio sources.
- Sample-accurate incoming audio playout timestamps to avoid jitter-induced metallic/distorted sound.
- Remote media routing by stable Talk actor identity, with per-track diagnostics.
- Private off-screen capture of the selected OBS device, scene, Preview, or Program source.
- Direct capture of the selected OBS Audio Input Capture source before monitor output.
- 1280x720 maximum, 30 fps VP8 publication and 48 kHz stereo Opus publication.
- MCU publishing plus peer-to-peer sender track negotiation.
- Talk-compatible `simplewebrtc` and `status` data channels with initial media-state announcements.
- Talk-compatible Olm key exchange and AES-GCM encoded-frame E2EE for incoming and outgoing audio/video.
- Explicit high spatial and temporal simulcast selection for every MCU subscriber, avoiding a participant being left at approximately 15 fps when several cameras are active.
- Incoming VP8 RTX recovery, ordered frame release, and RTP-clocked video playout so delayed keyframe fragments do not cause periodic freezes or bursty rendering.
- Incoming video pipelines reset their SSRC selection, decoder continuity, and RTP playout clock when a participant turns their camera off and back on.
- Participant video sources flush OBS's stale async-frame queue on camera-off and use real-time unbuffered delivery, so the first new keyframe is displayed immediately after camera-on.
- OBS shutdown safely tolerates the frontend destroying the dock before module unload.
- Participant placeholders are emitted only on a real online/offline transition or before the first decoded frame, preventing the two-second participant poll from flashing the grey line pattern over live video.
- Participant call leave/rejoin events preserve and renegotiate an MCU subscriber when the signaling session is unchanged; genuinely replaced sessions retire stale peers, offer markers, and E2EE state before binding the persistent OBS sources to the replacement.
- Transient participant-poll failures keep the active call state and retry automatically instead of exposing an incorrect second Join action.
- Talk audio-and-video call flags are set when starting or joining and confirmed once both encoders start.
- Selected outgoing mode, device sources, and scene survive OBS source teardown and restart.

Not yet implemented:

- Live source replacement while already in a call.
- A complete echo-cancellation/loop prevention graph.
- Nextcloud Login Flow v2 and server certificate/trust UI.
- Internal-signaling fallback, reconnect/resume, and live High Performance Backend compatibility testing.

The dock reports each signaling/WebRTC phase. Outgoing VP8 publication and incoming H.264/VP8 video plus Opus audio rendering are active in this milestone.

## Download and install

Download the Windows x64 package from the [0.7.0 release](https://github.com/menacius/obs-nextcloud-talk/releases/tag/v0.7.0) or the [OmniaTV Software page](https://software.omniatv.com/nextcloud-talk/).

Close OBS Studio, extract the package into the OBS Studio installation directory, and restart OBS. The archive contains the native plugin, its required runtime libraries, and the plugin data files in the standard OBS directory layout.

## Build on Windows

Requirements:

- Visual Studio 2022 with Desktop C++ support.
- CMake 3.24 or later.
- A configured OBS Studio build tree with exported `libobs` and frontend API packages.
- The matching OBS Qt 6 dependency bundle.
- The matching dependency bundle must provide libdatachannel with media enabled.
- The matching OBS FFmpeg runtime must provide VP8 (`libvpx`) and Opus (`libopus`) encoders.
- On Windows, the build automatically bundles Qt's native Schannel TLS backend under `tls/`.
- On Windows, the build bundles `datachannel.dll`; signaling uses the system WinHTTP TLS stack.

On the development layout used for this repository:

```powershell
.\build-windows.ps1
```

Or provide paths explicitly:

```powershell
.\build-windows.ps1 `
  -ObsSource C:\src\obs-studio `
  -ObsBuild C:\src\obs-studio\plugin_build_x64 `
  -Dependencies C:\deps\plugin-deps-qt6-x64
```

The plugin is produced at:

```text
build\RelWithDebInfo\obs-nextcloud-talk.dll
```

Run the tests with the matching Qt runtime on `PATH`:

```powershell
$env:PATH = "C:\path\to\qt-dependencies\bin;$env:PATH"
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

## OBS installation layout

Copy the built artifacts into an OBS installation or staging directory:

```text
obs-plugins/64bit/obs-nextcloud-talk.dll
obs-plugins/64bit/datachannel.dll
obs-plugins/64bit/tls/qschannelbackend.dll
data/obs-plugins/obs-nextcloud-talk/locale/en-US.ini
```

Do not distribute only the DLL if future media-engine dependencies are enabled; their runtime libraries and licenses must be packaged too.

## Source map

- `src/talk-client.*` — Nextcloud OCS call control and response parsing.
- `src/talk-media-transport.*` — Talk external signaling and WebRTC peer sessions.
- `src/outgoing-media.*` — selected-source capture, resampling, H.264, and Opus encoding.
- `src/incoming-media-decoder.*` — FFmpeg video/audio decode and OBS frame delivery.
- `src/winhttp-websocket.*` — native Windows secure WebSocket transport.
- `src/source-registry.*` — stable participant-to-OBS-source lifecycle.
- `src/participant-source.*` — native asynchronous OBS video/audio inputs and media frame ingress.
- `src/talk-dock.*` — account, conversation, participant, and outgoing-media controls.
- `src/config.*` — non-secret settings persistence.
- `src/credential-store.*` — OS-protected app-password persistence.
- `docs/architecture.md` — transport boundary and next implementation stages.

## Security notes

- Use a Nextcloud app password, not the account's primary password.
- The plugin does not bypass TLS errors.
- The app password is stored as a generic Windows credential, encrypted and access-controlled by Windows for the signed-in account. It is never written to `settings.json`.
- Use **Forget saved** in the dock to remove the stored credential.
- OBS scenes may contain participant names in source names; treat scene collections as personal data.

## License

This implementation is intended to be distributed under GPL-2.0-or-later, compatible with OBS Studio. A complete dependency and license review is required before distributing a build with a WebRTC library.
