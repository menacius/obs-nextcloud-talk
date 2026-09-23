# Architecture and media transport boundary

## Implemented control plane

```text
TalkDock
  ├─ TalkClient
  │    ├─ GET  /ocs/v2.php/apps/spreed/api/v4/room
  │    ├─ POST /ocs/v2.php/apps/spreed/api/v4/call/{token}
  │    ├─ PUT  /ocs/v2.php/apps/spreed/api/v4/call/{token} (media flags)
  │    ├─ GET  /ocs/v2.php/apps/spreed/api/v4/call/{token}
  │    └─ DELETE /ocs/v2.php/apps/spreed/api/v4/call/{token}
  ├─ TalkMediaTransport
  │    ├─ WinHTTP WebSocket ─► external Talk signaling / HPB
  │    └─ libdatachannel ─► ICE, DTLS, SRTP, send/receive tracks
  ├─ OutgoingMedia
  │    ├─ private OBS view ─► I420 ─► H.264
  │    └─ selected audio callback ─► 48 kHz stereo ─► Opus
  └─ SourceRegistry
       ├─ Talk - {participant} - Video
       └─ Talk - {participant} - Audio
```

`SourceRegistry` keys participants by `actorType:actorId` and only falls back to `sessionId` when no actor ID exists. Sources are marked offline instead of being destroyed, which preserves OBS scene items and filters across brief disconnects.

The video source emits a colored placeholder until decoded frames arrive. The audio source is silent until decoded PCM arrives. Incoming H.264/VP8 video and Opus audio are decoded and delivered through:

```text
publishParticipantVideo(participantId, frame)
publishParticipantAudio(participantId, frame)
```

## Media plane

The transport owns external signaling, WebRTC state, outgoing publication, and per-participant incoming decode:

```text
Nextcloud signaling / HPB
          ↕
 TalkMediaTransport
   ├─ remote decoded video ─► publishParticipantVideo()
   ├─ remote decoded audio ─► publishParticipantAudio()
   ├─ H.264 RTP publication ◄─ selected private OBS video view
   └─ Opus RTP publication ◄─ selected OBS audio-input callback
```

The signaling mode is discovered from Talk's `/signaling/settings` endpoint:

1. External signaling / High Performance Backend over WebSocket is implemented.
2. Internal signaling fallback for smaller installations remains to be implemented.

External signaling alone is not sufficient for broad compatibility.

## Incoming media rules

- A remote participant is never mixed with another participant before it reaches OBS.
- Video timestamps must use the OBS nanosecond clock domain before calling `obs_source_output_video`.
- Audio should be normalized to an OBS-supported format, preferably 48 kHz float planar, without combining participants.
- Track mute/unmute changes availability state; they do not replace the OBS source.
- A reconnect that retains actor identity reuses the same source.
- Simulcast layer selection belongs in the transport and should respond to the largest active OBS render size when practical.

## Outgoing video rules

The selected device source is rendered into a private off-screen OBS view. It is scaled to a maximum of 1280x720 at 30 fps, encoded as low-latency Annex-B H.264, packetized for WebRTC, and never added to Program or Preview. Live selection changes while already in a call remain future work.

The outgoing capture must not add the private view to the program scene and must release graphics resources on the OBS graphics thread.

## Outgoing audio and feedback rules

The current implementation captures the selected OBS Audio Input Capture source before monitor output, converts OBS planar float audio to 48 kHz stereo, and encodes 20 ms Opus packets. A dedicated OBS mix remains a future option.

There are two architectural targets:

1. A selected OBS audio source captured before monitor output.
2. A dedicated OBS mix captured through an output callback.

At minimum, every source whose type is `nextcloud_talk_participant_audio` must be excluded from the outgoing Talk mix. A stronger implementation should tag Talk-originated audio frames and reject them at the publication boundary even when they pass through nested scenes or routing plugins.

Echo cancellation should be optional. Broadcast workflows often require deterministic audio with no browser-style processing; conversational workflows may prefer WebRTC AEC/NS/AGC.

## Authentication

The client stores the app password as a generic Windows credential under a hashed server/user identity. OBS `settings.json` contains no password. Some Nextcloud configurations require a browser-backed session cookie for joining a call. A later implementation should add Login Flow v2, retain cookies only in memory, and clearly distinguish authentication errors from call permission/lobby errors.

## Delivery milestones

1. **Foundation (implemented):** module, dock, settings, API parsing/control, participant source lifecycle, tests.
2. **External signaling and WebRTC foundation (implemented):** settings discovery, WebSocket auth/room events, ICE/TURN, peer/MCU subscriber sessions, encoded RTP ingress.
3. **Incoming media (implemented for external signaling):** H.264/VP8/Opus depacketization and decode, timestamp conversion, and independent OBS frame/audio delivery. Internal signaling fallback remains future work.
4. **Outgoing media (initial publication implemented):** selected-source off-screen video and direct audio capture, H.264/Opus encoding, MCU and peer-to-peer sender tracks.
5. **Hardening:** reconnect/resume, lobby and consent UX, E2EE capability handling, stats, diagnostics, packaging and CI.
