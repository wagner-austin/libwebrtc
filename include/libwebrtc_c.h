#ifndef LIB_WEBRTC_C_H
#define LIB_WEBRTC_C_H

#include <cstddef>
#include <cstdint>

#include "rtc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lwrtc_factory lwrtc_factory_t;
typedef struct lwrtc_constraints lwrtc_constraints_t;
typedef struct lwrtc_peer lwrtc_peer_t;
typedef struct lwrtc_audio_source lwrtc_audio_source_t;
typedef struct lwrtc_video_source lwrtc_video_source_t;
typedef struct lwrtc_encoded_video_source lwrtc_encoded_video_source_t;
typedef struct lwrtc_audio_track lwrtc_audio_track_t;
typedef struct lwrtc_video_track lwrtc_video_track_t;
typedef struct lwrtc_data_channel lwrtc_data_channel_t;

// Video codec types for encoded video source
typedef enum lwrtc_video_codec {
  LWRTC_VIDEO_CODEC_H264 = 0,
  LWRTC_VIDEO_CODEC_H265 = 1,
  LWRTC_VIDEO_CODEC_AV1 = 2,
} lwrtc_video_codec_t;

typedef struct lwrtc_config {
  int offer_to_receive_audio;
  int offer_to_receive_video;
} lwrtc_config_t;

typedef void (*lwrtc_ice_cb)(
    void* user,
    const char* mid,
    int mline_index,
    const char* candidate);
// Fires when the RTCPeerConnection state transitions. The `state` value
// matches the RTCPeerConnectionState enum in rtc_peerconnection.h —
// {Failed, Closed} indicate the peer will not recover and callers should
// tear down the session (input capture, media pipelines, etc.). Wired
// through the observer at lwrtc_peer_set_connection_state_cb.
typedef void (*lwrtc_peer_connection_state_cb)(void* user, int state);
typedef void (*lwrtc_data_channel_cb)(void* user, lwrtc_data_channel_t* channel);
typedef void (*lwrtc_data_channel_state_cb)(void* user, int state);
typedef void (*lwrtc_data_channel_message_cb)(
    void* user,
    const char* buffer,
    int length,
    int binary);
// sdp/type strings are valid only for the duration of the callback.
typedef void (*lwrtc_sdp_cb)(void* user, const char* sdp, const char* type);
typedef void (*lwrtc_ok_cb)(void* user);
typedef void (*lwrtc_err_cb)(void* user, const char* error);

// Callback for keyframe (IDR) requests triggered by PLI/FIR from receiver
typedef void (*lwrtc_keyframe_request_cb)(void* user);

// Callback for releasing externally owned encoded frame buffers.
// Called exactly once when WebRTC no longer needs the buffer (or immediately if
// the frame is dropped before entering the pipeline).
typedef void (*lwrtc_buffer_release_cb)(void* user);

typedef enum lwrtc_data_channel_state {
  LWRTC_DATA_CHANNEL_CONNECTING = 0,
  LWRTC_DATA_CHANNEL_OPEN = 1,
  LWRTC_DATA_CHANNEL_CLOSING = 2,
  LWRTC_DATA_CHANNEL_CLOSED = 3,
} lwrtc_data_channel_state_t;

LIB_WEBRTC_API int lwrtc_initialize();
LIB_WEBRTC_API void lwrtc_terminate();

LIB_WEBRTC_API lwrtc_factory_t* lwrtc_factory_create();
LIB_WEBRTC_API int lwrtc_factory_initialize(lwrtc_factory_t* factory);

// Enable passthrough mode for the factory.
// Must be called BEFORE lwrtc_factory_initialize().
// When enabled, the factory will use the PassthroughVideoEncoder
// which accepts pre-encoded H.264/HEVC/AV1 frames.
LIB_WEBRTC_API int lwrtc_factory_enable_passthrough(
    lwrtc_factory_t* factory,
    lwrtc_video_codec_t codec);
// Optional AV1 fmtp overrides used to match remote offers.
// Must be called BEFORE lwrtc_factory_initialize().
LIB_WEBRTC_API int lwrtc_factory_set_passthrough_av1_params(
    lwrtc_factory_t* factory,
    const char* profile,
    const char* level_idx,
    const char* tier);
// Optional HEVC fmtp overrides used to match remote offers.
// Must be called BEFORE lwrtc_factory_initialize().
LIB_WEBRTC_API int lwrtc_factory_set_passthrough_hevc_fmtp(
    lwrtc_factory_t* factory,
    const char* fmtp);
// Build the factory over webrtc's dummy audio device instead of the platform
// one (Windows Core Audio): no audio endpoint is opened at initialization or
// when an audio stream is added. For a caller that plays nothing and pushes
// its own captured audio through lwrtc_audio_source_push.
// Must be called BEFORE lwrtc_factory_initialize().
LIB_WEBRTC_API int lwrtc_factory_use_dummy_audio_device(
    lwrtc_factory_t* factory);

// How long each step of lwrtc_factory_initialize() took, in microseconds.
// A step the initialization skipped is 0 (ssl_us after the first factory of
// the process).
typedef struct lwrtc_factory_init_timings {
  int64_t ssl_us;
  int64_t threads_us;
  int64_t audio_device_create_us;
  int64_t audio_device_init_us;
  int64_t audio_processing_us;
  int64_t peer_connection_factory_us;
} lwrtc_factory_init_timings_t;
// Fills `timings` for a factory lwrtc_factory_initialize() built. Returns 0,
// leaving `timings` untouched, when the factory is not initialized.
LIB_WEBRTC_API int lwrtc_factory_get_init_timings(
    const lwrtc_factory_t* factory,
    lwrtc_factory_init_timings_t* timings);
LIB_WEBRTC_API void lwrtc_factory_release(lwrtc_factory_t* factory);

LIB_WEBRTC_API lwrtc_constraints_t* lwrtc_constraints_create();
LIB_WEBRTC_API int lwrtc_constraints_add_mandatory(
    lwrtc_constraints_t* constraints,
    const char* key,
    const char* value);
LIB_WEBRTC_API void lwrtc_constraints_release(lwrtc_constraints_t* constraints);

LIB_WEBRTC_API lwrtc_peer_t* lwrtc_factory_create_peer(
    lwrtc_factory_t* factory,
    const lwrtc_config_t* config,
    lwrtc_constraints_t* constraints,
    lwrtc_ice_cb ice_cb,
    void* user);

LIB_WEBRTC_API void lwrtc_peer_close(lwrtc_peer_t* peer);
LIB_WEBRTC_API void lwrtc_peer_release(lwrtc_peer_t* peer);

// Register a callback fired on RTCPeerConnection state changes. libwebrtc
// transitions the state to Failed after ICE consent-freshness lapses per
// RFC 7675 (~15s default) when the remote peer is unreachable, and to
// Closed on explicit teardown. Passing cb=nullptr clears the registration.
// Safe to call multiple times; last registration wins.
LIB_WEBRTC_API void lwrtc_peer_set_connection_state_cb(
    lwrtc_peer_t* peer,
    void* user,
    lwrtc_peer_connection_state_cb cb);

LIB_WEBRTC_API void lwrtc_peer_set_remote_description(
    lwrtc_peer_t* peer,
    const char* sdp,
    const char* type,
    lwrtc_ok_cb on_ok,
    lwrtc_err_cb on_err,
    void* user);

LIB_WEBRTC_API void lwrtc_peer_create_answer(
    lwrtc_peer_t* peer,
    lwrtc_sdp_cb on_ok,
    lwrtc_err_cb on_err,
    lwrtc_constraints_t* constraints,
    void* user);

LIB_WEBRTC_API void lwrtc_peer_set_local_description(
    lwrtc_peer_t* peer,
    const char* sdp,
    const char* type,
    lwrtc_ok_cb on_ok,
    lwrtc_err_cb on_err,
    void* user);

LIB_WEBRTC_API void lwrtc_peer_add_candidate(
    lwrtc_peer_t* peer,
    const char* mid,
    int mline_index,
    const char* candidate);

LIB_WEBRTC_API void lwrtc_peer_register_data_channel(
    lwrtc_peer_t* peer,
    lwrtc_data_channel_cb on_channel,
    void* user);

LIB_WEBRTC_API lwrtc_data_channel_t* lwrtc_peer_create_data_channel(
    lwrtc_peer_t* peer,
    const char* label);

LIB_WEBRTC_API lwrtc_audio_source_t* lwrtc_audio_source_create(
    lwrtc_factory_t* factory);
LIB_WEBRTC_API void lwrtc_audio_source_release(lwrtc_audio_source_t* source);
LIB_WEBRTC_API int lwrtc_audio_source_push(
    lwrtc_audio_source_t* source,
    const void* audio_data,
    int bits_per_sample,
    int sample_rate,
    size_t number_of_channels,
    size_t number_of_frames);

LIB_WEBRTC_API lwrtc_video_source_t* lwrtc_video_source_create(
    lwrtc_factory_t* factory);
LIB_WEBRTC_API void lwrtc_video_source_release(lwrtc_video_source_t* source);
LIB_WEBRTC_API int lwrtc_video_source_push_i420(
    lwrtc_video_source_t* source,
    const uint8_t* data_y,
    int stride_y,
    const uint8_t* data_u,
    int stride_u,
    const uint8_t* data_v,
    int stride_v,
    int width,
    int height,
    int64_t timestamp_us);

LIB_WEBRTC_API int lwrtc_video_source_push_nv12(
    lwrtc_video_source_t* source,
    const uint8_t* data_y,
    int stride_y,
    const uint8_t* data_uv,
    int stride_uv,
    int width,
    int height,
    int64_t timestamp_us);
LIB_WEBRTC_API int lwrtc_video_source_push_argb(
    lwrtc_video_source_t* source,
    const uint8_t* data,
    int stride,
    int width,
    int height,
    int64_t timestamp_us);

// Windows-only: push a D3D11 texture directly as a native video frame.
// texture: ID3D11Texture2D*, keyed_mutex: IDXGIKeyedMutex* (may be null).
LIB_WEBRTC_API int lwrtc_video_source_push_d3d11(
    lwrtc_video_source_t* source,
    void* texture,
    void* keyed_mutex,
    int width,
    int height,
    int64_t timestamp_us);

LIB_WEBRTC_API lwrtc_audio_track_t* lwrtc_audio_track_create(
    lwrtc_factory_t* factory,
    lwrtc_audio_source_t* source,
    const char* track_id);
LIB_WEBRTC_API lwrtc_video_track_t* lwrtc_video_track_create(
    lwrtc_factory_t* factory,
    lwrtc_video_source_t* source,
    const char* track_id);
LIB_WEBRTC_API void lwrtc_audio_track_release(lwrtc_audio_track_t* track);
LIB_WEBRTC_API void lwrtc_video_track_release(lwrtc_video_track_t* track);

LIB_WEBRTC_API int lwrtc_peer_add_audio_track(
    lwrtc_peer_t* peer,
    lwrtc_audio_track_t* track,
    const char* stream_id);
LIB_WEBRTC_API int lwrtc_peer_add_video_track(
    lwrtc_peer_t* peer,
    lwrtc_video_track_t* track,
    const char* stream_id);

LIB_WEBRTC_API void lwrtc_data_channel_register_observer(
    lwrtc_data_channel_t* channel,
    lwrtc_data_channel_state_cb on_state,
    lwrtc_data_channel_message_cb on_message,
    void* user);
LIB_WEBRTC_API void lwrtc_data_channel_unregister_observer(
    lwrtc_data_channel_t* channel);
LIB_WEBRTC_API const char* lwrtc_data_channel_label(
    lwrtc_data_channel_t* channel);
LIB_WEBRTC_API int lwrtc_data_channel_state(lwrtc_data_channel_t* channel);
LIB_WEBRTC_API int lwrtc_data_channel_send(
    lwrtc_data_channel_t* channel,
    const uint8_t* data,
    size_t size,
    int binary);
LIB_WEBRTC_API void lwrtc_data_channel_close(lwrtc_data_channel_t* channel);
LIB_WEBRTC_API void lwrtc_data_channel_release(lwrtc_data_channel_t* channel);

// Encoded video source API - for sending pre-encoded H.264/HEVC frames
// without re-encoding (passthrough mode)

// Create an encoded video source with passthrough encoder.
// codec: LWRTC_VIDEO_CODEC_H264, LWRTC_VIDEO_CODEC_H265, or LWRTC_VIDEO_CODEC_AV1
// width/height: initial video dimensions (can be updated via push)
LIB_WEBRTC_API lwrtc_encoded_video_source_t* lwrtc_encoded_video_source_create(
    lwrtc_factory_t* factory,
    lwrtc_video_codec_t codec,
    int width,
    int height);

// Release an encoded video source.
LIB_WEBRTC_API void lwrtc_encoded_video_source_release(
    lwrtc_encoded_video_source_t* source);

// Push a pre-encoded frame to the WebRTC pipeline.
// data: NAL unit data in Annex B format (with start codes)
// size: size of the data in bytes
// timestamp_us: capture timestamp in microseconds
// is_keyframe: 1 if this is an IDR frame, 0 otherwise
// Returns 1 on success, 0 on failure.
LIB_WEBRTC_API int lwrtc_encoded_video_source_push(
    lwrtc_encoded_video_source_t* source,
    const uint8_t* data,
    size_t size,
    int64_t timestamp_us,
    int is_keyframe);

// Push a pre-encoded frame to the WebRTC pipeline without copying the payload.
// The caller retains ownership of `data` until `release_cb` is invoked.
// Returns 1 on success, 0 on failure.
LIB_WEBRTC_API int lwrtc_encoded_video_source_push_shared(
    lwrtc_encoded_video_source_t* source,
    const uint8_t* data,
    size_t size,
    int64_t timestamp_us,
    int is_keyframe,
    lwrtc_buffer_release_cb release_cb,
    void* release_user);

// Set callback for keyframe requests (PLI/FIR from receiver).
// When called, the application should request an IDR from the upstream encoder.
LIB_WEBRTC_API void lwrtc_encoded_video_source_set_keyframe_callback(
    lwrtc_encoded_video_source_t* source,
    lwrtc_keyframe_request_cb cb,
    void* user);

// Create a video track from an encoded video source.
LIB_WEBRTC_API lwrtc_video_track_t* lwrtc_encoded_video_track_create(
    lwrtc_factory_t* factory,
    lwrtc_encoded_video_source_t* source,
    const char* track_id);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LIB_WEBRTC_C_H
