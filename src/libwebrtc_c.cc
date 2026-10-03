#include "libwebrtc_c.h"

#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "api/scoped_refptr.h"
#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_rotation.h"
#include "api/video_codecs/sdp_video_format.h"
#include "base/refcountedobject.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/time_utils.h"
#include "libwebrtc.h"
#include "rtc_ice_candidate.h"
#include "rtc_mediaconstraints.h"
#include "rtc_data_channel.h"
#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"
#include "rtc_peerconnection_factory_impl.h"
#include "rtc_video_source_impl.h"
#include "src/internal/video_capturer.h"
#include "src/internal/vcm_capturer.h"
#include "src/passthrough_video_encoder.h"
#include "third_party/libyuv/include/libyuv.h"

namespace {
webrtc::VideoCodecType ToWebrtcCodec(lwrtc_video_codec_t codec) {
  switch (codec) {
    case LWRTC_VIDEO_CODEC_H265:
      return webrtc::kVideoCodecH265;
    case LWRTC_VIDEO_CODEC_AV1:
      return webrtc::kVideoCodecAV1;
    case LWRTC_VIDEO_CODEC_H264:
    default:
      return webrtc::kVideoCodecH264;
  }
}

std::string_view TrimAscii(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

webrtc::CodecParameterMap ParseFmtpParameters(std::string_view fmtp) {
  webrtc::CodecParameterMap params;
  std::size_t token_start = 0;
  while (token_start < fmtp.size()) {
    std::size_t token_end = fmtp.find(';', token_start);
    if (token_end == std::string_view::npos) {
      token_end = fmtp.size();
    }
    auto token = TrimAscii(fmtp.substr(token_start, token_end - token_start));
    if (!token.empty()) {
      const std::size_t eq = token.find('=');
      if (eq != std::string_view::npos) {
        auto key = TrimAscii(token.substr(0, eq));
        auto value = TrimAscii(token.substr(eq + 1));
        if (!key.empty()) {
          params[std::string {key}] = std::string {value};
        }
      }
    }
    token_start = token_end + 1;
  }
  return params;
}
}  // namespace

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi.h>
#include "src/win/d3d11_texture_buffer.h"
#endif

class DataChannelObserver;
class PeerObserver;
class PushVideoCapturer;

struct lwrtc_factory {
  libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> handle;
  // Passthrough encoder factory (stored here so we can access the encoder)
  owt::base::PassthroughVideoEncoderFactory* passthrough_factory = nullptr;
  lwrtc_video_codec_t passthrough_codec = LWRTC_VIDEO_CODEC_H264;
  bool use_passthrough = false;
  bool use_dummy_audio_device = false;
  // LibWebRTC::Initialize() inside lwrtc_factory_initialize().
  int64_t ssl_us = 0;
  std::optional<webrtc::CodecParameterMap> hevc_parameters;
  std::optional<std::string> av1_profile;
  std::optional<std::string> av1_level_idx;
  std::optional<std::string> av1_tier;
};

struct lwrtc_constraints {
  libwebrtc::scoped_refptr<libwebrtc::RTCMediaConstraints> handle;
};

struct lwrtc_peer {
  libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> handle;
  std::unique_ptr<PeerObserver> observer;
  lwrtc_ice_cb ice_cb = nullptr;
  void* ice_user = nullptr;
  lwrtc_data_channel_cb data_channel_cb = nullptr;
  void* data_channel_user = nullptr;
  lwrtc_peer_connection_state_cb pc_state_cb = nullptr;
  void* pc_state_user = nullptr;
};

struct lwrtc_audio_source {
  libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source;
};

struct lwrtc_video_source {
  std::shared_ptr<PushVideoCapturer> capturer;
  libwebrtc::scoped_refptr<libwebrtc::RTCVideoSource> source;
};

struct lwrtc_encoded_video_source {
  lwrtc_factory_t* factory = nullptr;
  lwrtc_video_codec_t codec = LWRTC_VIDEO_CODEC_H264;
  int width = 0;
  int height = 0;
  lwrtc_keyframe_request_cb keyframe_cb = nullptr;
  void* keyframe_user = nullptr;
  // Capturer for pushing dummy frames to trigger encoder creation
  std::shared_ptr<PushVideoCapturer> capturer;
  bool encoder_ready = false;
  int64_t last_dummy_push_us = 0;
  rtc::scoped_refptr<webrtc::I420Buffer> dummy_buffer;
  int dummy_width = 0;
  int dummy_height = 0;
};

struct lwrtc_audio_track {
  libwebrtc::scoped_refptr<libwebrtc::RTCAudioTrack> track;
};

struct lwrtc_video_track {
  libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> track;
};

struct lwrtc_data_channel {
  libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel> handle;
  std::unique_ptr<DataChannelObserver> observer;
  std::string label;
};

class DataChannelObserver final : public libwebrtc::RTCDataChannelObserver {
 public:
  DataChannelObserver(lwrtc_data_channel_state_cb state_cb,
                      lwrtc_data_channel_message_cb message_cb,
                      void* user)
      : state_cb_(state_cb), message_cb_(message_cb), user_(user) {}

  void OnStateChange(libwebrtc::RTCDataChannelState state) override {
    if (state_cb_) {
      state_cb_(user_, static_cast<int>(state));
    }
  }

  void OnMessage(const char* buffer, int length, bool binary) override {
    if (message_cb_) {
      message_cb_(user_, buffer, length, binary ? 1 : 0);
    }
  }

 private:
  lwrtc_data_channel_state_cb state_cb_;
  lwrtc_data_channel_message_cb message_cb_;
  void* user_;
};

class PeerObserver final : public libwebrtc::RTCPeerConnectionObserver {
 public:
  explicit PeerObserver(lwrtc_peer* peer) : peer_(peer) {}

  void OnSignalingState(libwebrtc::RTCSignalingState) override {}
  void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState state) override {
    // Forward the standardized peer-connection-state transition to a wrapper
    // consumer if one is registered. libwebrtc emits Failed after ICE
    // consent-freshness lapses per RFC 7675 (~15s default) when the remote
    // peer becomes unreachable, and Closed on explicit teardown. Without
    // this forwarder the signal was discarded — downstream projects had
    // no way to observe peer death and could not release captured
    // resources tied to the session lifetime.
    if (peer_ && peer_->pc_state_cb) {
      peer_->pc_state_cb(peer_->pc_state_user, static_cast<int>(state));
    }
  }
  void OnIceGatheringState(libwebrtc::RTCIceGatheringState) override {}
  void OnIceConnectionState(libwebrtc::RTCIceConnectionState) override {}

  void OnIceCandidate(
      libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate> candidate) override {
    if (!peer_ || !peer_->ice_cb || !candidate) {
      return;
    }
    std::string mid = candidate->sdp_mid().c_string();
    std::string cand = candidate->candidate().c_string();
    peer_->ice_cb(peer_->ice_user, mid.c_str(), candidate->sdp_mline_index(),
                  cand.c_str());
  }

  void OnAddStream(
      libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
  void OnRemoveStream(
      libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
  void OnDataChannel(
      libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel> channel) override {
    if (!peer_ || !peer_->data_channel_cb || !channel) {
      return;
    }
    auto* wrapped = new (std::nothrow) lwrtc_data_channel();
    if (!wrapped) {
      return;
    }
    wrapped->handle = channel;
    wrapped->label = channel->label().c_string();
    peer_->data_channel_cb(peer_->data_channel_user, wrapped);
  }
  void OnRenegotiationNeeded() override {}
  void OnTrack(
      libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver>) override {}
  void OnAddTrack(
      libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>>,
      libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}
  void OnRemoveTrack(
      libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}

 private:
  lwrtc_peer* peer_;
};

class PushVideoCapturer : public webrtc::internal::VideoCapturer {
 public:
  void PushFrame(const webrtc::VideoFrame& frame) { OnFrame(frame); }
};

extern "C" {

int lwrtc_initialize() {
  return libwebrtc::LibWebRTC::Initialize() ? 1 : 0;
}

void lwrtc_terminate() {
  libwebrtc::LibWebRTC::Terminate();
}

lwrtc_factory_t* lwrtc_factory_create() {
  return new (std::nothrow) lwrtc_factory();
}

int lwrtc_factory_initialize(lwrtc_factory_t* factory) {
  if (!factory) {
    return 0;
  }
  if (factory->handle) {
    return 1;
  }
  const int64_t ssl_start_us = rtc::TimeMicros();
  if (!libwebrtc::LibWebRTC::Initialize()) {
    return 0;
  }
  factory->ssl_us = rtc::TimeMicros() - ssl_start_us;
  factory->handle = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
  if (!factory->handle) {
    return 0;
  }

  if (factory->use_dummy_audio_device) {
    static_cast<libwebrtc::RTCPeerConnectionFactoryImpl*>(factory->handle.get())
        ->UseDummyAudioDevice();
  }

  // If passthrough mode is enabled, set the custom encoder factory
  if (factory->use_passthrough) {
    // Get the underlying implementation to set the encoder factory
    auto* impl = static_cast<libwebrtc::RTCPeerConnectionFactoryImpl*>(
        factory->handle.get());
    if (impl) {
      // We need to give the RTCPeerConnectionFactory a unique_ptr,
      // but we also need to keep access to the encoder.
      // We'll use GetVideoEncoderFactory() after Initialize() to get
      // the encoder from the factory we pass in.
      auto passthrough_factory =
          std::make_unique<owt::base::PassthroughVideoEncoderFactory>();
      passthrough_factory->SetPreferredCodec(
          ToWebrtcCodec(factory->passthrough_codec));
      if (factory->hevc_parameters) {
        passthrough_factory->SetH265Parameters(factory->hevc_parameters);
      }
      if (factory->av1_profile || factory->av1_level_idx || factory->av1_tier) {
        webrtc::CodecParameterMap params;
        params["profile"] = factory->av1_profile.value_or("0");
        params["level-idx"] = factory->av1_level_idx.value_or("5");
        params["tier"] = factory->av1_tier.value_or("0");
        passthrough_factory->SetAv1Parameters(std::move(params));
      }
      factory->passthrough_factory = passthrough_factory.get();
      impl->SetVideoEncoderFactory(std::move(passthrough_factory));
    } else {
      factory->passthrough_factory = nullptr;
    }
  }

  if (!factory->handle->Initialize()) {
    factory->handle = nullptr;
    factory->passthrough_factory = nullptr;
    return 0;
  }
  return 1;
}

int lwrtc_factory_enable_passthrough(
    lwrtc_factory_t* factory,
    lwrtc_video_codec_t codec) {
  if (!factory) {
    return 0;
  }
  if (factory->handle) {
    // Already initialized - too late to enable passthrough
    return 0;
  }
  factory->use_passthrough = true;
  factory->passthrough_codec = codec;
  return 1;
}

int lwrtc_factory_use_dummy_audio_device(lwrtc_factory_t* factory) {
  if (!factory) {
    return 0;
  }
  if (factory->handle) {
    // Already initialized - the audio device is chosen at initialization
    return 0;
  }
  factory->use_dummy_audio_device = true;
  return 1;
}

int lwrtc_factory_get_init_timings(
    const lwrtc_factory_t* factory,
    lwrtc_factory_init_timings_t* timings) {
  if (!factory || !factory->handle || !timings) {
    return 0;
  }
  const auto& steps =
      static_cast<const libwebrtc::RTCPeerConnectionFactoryImpl*>(
          factory->handle.get())
          ->initialize_timings();
  timings->ssl_us = factory->ssl_us;
  timings->threads_us = steps.threads_us;
  timings->audio_device_create_us = steps.audio_device_create_us;
  timings->audio_device_init_us = steps.audio_device_init_us;
  timings->audio_processing_us = steps.audio_processing_us;
  timings->peer_connection_factory_us = steps.peer_connection_factory_us;
  return 1;
}

int lwrtc_factory_set_passthrough_av1_params(
    lwrtc_factory_t* factory,
    const char* profile,
    const char* level_idx,
    const char* tier) {
  if (!factory) {
    return 0;
  }
  if (factory->handle) {
    // Too late to update parameters after initialization.
    return 0;
  }

  if (profile && *profile) {
    factory->av1_profile = profile;
  } else {
    factory->av1_profile.reset();
  }
  if (level_idx && *level_idx) {
    factory->av1_level_idx = level_idx;
  } else {
    factory->av1_level_idx.reset();
  }
  if (tier && *tier) {
    factory->av1_tier = tier;
  } else {
    factory->av1_tier.reset();
  }
  return 1;
}

int lwrtc_factory_set_passthrough_hevc_fmtp(
    lwrtc_factory_t* factory,
    const char* fmtp) {
  if (!factory) {
    return 0;
  }
  if (factory->handle) {
    // Too late to update parameters after initialization.
    return 0;
  }

  if (!fmtp || !*fmtp) {
    factory->hevc_parameters.reset();
    return 1;
  }

  factory->hevc_parameters = ParseFmtpParameters(fmtp);
  return 1;
}

void lwrtc_factory_release(lwrtc_factory_t* factory) {
  delete factory;
}

lwrtc_constraints_t* lwrtc_constraints_create() {
  auto constraints = new (std::nothrow) lwrtc_constraints();
  if (!constraints) {
    return nullptr;
  }
  constraints->handle = libwebrtc::RTCMediaConstraints::Create();
  if (!constraints->handle) {
    delete constraints;
    return nullptr;
  }
  return constraints;
}

int lwrtc_constraints_add_mandatory(
    lwrtc_constraints_t* constraints,
    const char* key,
    const char* value) {
  if (!constraints || !constraints->handle || !key || !value) {
    return 0;
  }
  constraints->handle->AddMandatoryConstraint(key, value);
  return 1;
}

void lwrtc_constraints_release(lwrtc_constraints_t* constraints) {
  delete constraints;
}

lwrtc_peer_t* lwrtc_factory_create_peer(
    lwrtc_factory_t* factory,
    const lwrtc_config_t* config,
    lwrtc_constraints_t* constraints,
    lwrtc_ice_cb ice_cb,
    void* user) {
  if (!factory || !factory->handle) {
    return nullptr;
  }

  libwebrtc::RTCConfiguration rtc_config;
  if (config) {
    rtc_config.offer_to_receive_audio = config->offer_to_receive_audio != 0;
    rtc_config.offer_to_receive_video = config->offer_to_receive_video != 0;
  }

  libwebrtc::scoped_refptr<libwebrtc::RTCMediaConstraints> constraints_ref =
      constraints ? constraints->handle : nullptr;
  if (!constraints_ref) {
    constraints_ref = libwebrtc::RTCMediaConstraints::Create();
  }

  auto peer_handle = factory->handle->Create(rtc_config, constraints_ref);
  if (!peer_handle) {
    return nullptr;
  }

  auto peer = new (std::nothrow) lwrtc_peer();
  if (!peer) {
    return nullptr;
  }
  peer->handle = peer_handle;
  peer->ice_cb = ice_cb;
  peer->ice_user = user;
  peer->observer = std::make_unique<PeerObserver>(peer);
  peer->handle->RegisterRTCPeerConnectionObserver(peer->observer.get());
  return peer;
}

void lwrtc_peer_close(lwrtc_peer_t* peer) {
  if (!peer || !peer->handle) {
    return;
  }
  peer->handle->Close();
}

void lwrtc_peer_release(lwrtc_peer_t* peer) {
  if (!peer) {
    return;
  }
  if (peer->handle && peer->observer) {
    peer->handle->DeRegisterRTCPeerConnectionObserver();
  }
  peer->observer.reset();
  delete peer;
}

void lwrtc_peer_set_connection_state_cb(
    lwrtc_peer_t* peer,
    void* user,
    lwrtc_peer_connection_state_cb cb) {
  if (!peer) {
    return;
  }
  peer->pc_state_cb = cb;
  peer->pc_state_user = user;
}

void lwrtc_peer_set_remote_description(
    lwrtc_peer_t* peer,
    const char* sdp,
    const char* type,
    lwrtc_ok_cb on_ok,
    lwrtc_err_cb on_err,
    void* user) {
  if (!peer || !peer->handle || !sdp || !type) {
    if (on_err) {
      on_err(user, "Invalid arguments");
    }
    return;
  }

  peer->handle->SetRemoteDescription(
      sdp, type,
      [on_ok, user]() {
        if (on_ok) {
          on_ok(user);
        }
      },
      [on_err, user](const char* error) {
        if (on_err) {
          on_err(user, error);
        }
      });
}

void lwrtc_peer_create_answer(
    lwrtc_peer_t* peer,
    lwrtc_sdp_cb on_ok,
    lwrtc_err_cb on_err,
    lwrtc_constraints_t* constraints,
    void* user) {
  if (!peer || !peer->handle) {
    if (on_err) {
      on_err(user, "Invalid arguments");
    }
    return;
  }

  libwebrtc::scoped_refptr<libwebrtc::RTCMediaConstraints> constraints_ref =
      constraints ? constraints->handle : nullptr;
  if (!constraints_ref) {
    constraints_ref = libwebrtc::RTCMediaConstraints::Create();
  }

  peer->handle->CreateAnswer(
      [on_ok, user, constraints_ref](const libwebrtc::string sdp,
                                     const libwebrtc::string type) {
        if (on_ok) {
          on_ok(user, sdp.c_string(), type.c_string());
        }
      },
      [on_err, user, constraints_ref](const char* error) {
        if (on_err) {
          on_err(user, error);
        }
      },
      constraints_ref);
}

void lwrtc_peer_set_local_description(
    lwrtc_peer_t* peer,
    const char* sdp,
    const char* type,
    lwrtc_ok_cb on_ok,
    lwrtc_err_cb on_err,
    void* user) {
  if (!peer || !peer->handle || !sdp || !type) {
    if (on_err) {
      on_err(user, "Invalid arguments");
    }
    return;
  }

  peer->handle->SetLocalDescription(
      sdp, type,
      [on_ok, user]() {
        if (on_ok) {
          on_ok(user);
        }
      },
      [on_err, user](const char* error) {
        if (on_err) {
          on_err(user, error);
        }
      });
}

void lwrtc_peer_add_candidate(
    lwrtc_peer_t* peer,
    const char* mid,
    int mline_index,
    const char* candidate) {
  if (!peer || !peer->handle || !mid || !candidate) {
    return;
  }
  peer->handle->AddCandidate(mid, mline_index, candidate);
}

void lwrtc_peer_register_data_channel(
    lwrtc_peer_t* peer,
    lwrtc_data_channel_cb on_channel,
    void* user) {
  if (!peer) {
    return;
  }
  peer->data_channel_cb = on_channel;
  peer->data_channel_user = user;
}

lwrtc_data_channel_t* lwrtc_peer_create_data_channel(
    lwrtc_peer_t* peer,
    const char* label) {
  if (!peer || !peer->handle || !label) {
    return nullptr;
  }

  auto channel = peer->handle->CreateDataChannel(label, nullptr);
  if (!channel) {
    return nullptr;
  }

  auto* wrapped = new (std::nothrow) lwrtc_data_channel();
  if (!wrapped) {
    return nullptr;
  }
  wrapped->handle = channel;
  wrapped->label = channel->label().c_string();
  return wrapped;
}

lwrtc_audio_source_t* lwrtc_audio_source_create(lwrtc_factory_t* factory) {
  if (!factory || !factory->handle) {
    return nullptr;
  }

  auto source = factory->handle->CreateAudioSource(
      libwebrtc::string("sunshine_audio"), libwebrtc::RTCAudioSource::kCustom);
  if (!source) {
    return nullptr;
  }

  auto* handle = new (std::nothrow) lwrtc_audio_source();
  if (!handle) {
    return nullptr;
  }
  handle->source = source;
  return handle;
}

void lwrtc_audio_source_release(lwrtc_audio_source_t* source) {
  delete source;
}

int lwrtc_audio_source_push(
    lwrtc_audio_source_t* source,
    const void* audio_data,
    int bits_per_sample,
    int sample_rate,
    size_t number_of_channels,
    size_t number_of_frames) {
  if (!source || !source->source || !audio_data) {
    return 0;
  }
  source->source->CaptureFrame(audio_data, bits_per_sample, sample_rate,
                               number_of_channels, number_of_frames);
  return 1;
}

lwrtc_video_source_t* lwrtc_video_source_create(lwrtc_factory_t* factory) {
  if (!factory || !factory->handle) {
    return nullptr;
  }

  auto capturer = std::make_shared<PushVideoCapturer>();
  auto rtc_track_source = webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>(
      new rtc::RefCountedObject<webrtc::internal::CapturerTrackSource>(capturer));
  auto source =
      libwebrtc::scoped_refptr<libwebrtc::RTCVideoSourceImpl>(
          new libwebrtc::RefCountedObject<libwebrtc::RTCVideoSourceImpl>(rtc_track_source));

  auto* handle = new (std::nothrow) lwrtc_video_source();
  if (!handle) {
    return nullptr;
  }
  handle->capturer = std::move(capturer);
  handle->source = std::move(source);
  return handle;
}

void lwrtc_video_source_release(lwrtc_video_source_t* source) {
  delete source;
}

int lwrtc_video_source_push_i420(
    lwrtc_video_source_t* source,
    const uint8_t* data_y,
    int stride_y,
    const uint8_t* data_u,
    int stride_u,
    const uint8_t* data_v,
    int stride_v,
    int width,
    int height,
    int64_t timestamp_us) {
  if (!source || !source->capturer || !data_y || !data_u || !data_v ||
      width <= 0 || height <= 0) {
    return 0;
  }

  auto buffer = webrtc::I420Buffer::Copy(
      width, height, data_y, stride_y, data_u, stride_u, data_v, stride_v);
  if (!buffer) {
    return 0;
  }

  auto frame = webrtc::VideoFrame::Builder()
                   .set_video_frame_buffer(buffer)
                   .set_rotation(webrtc::kVideoRotation_0)
                   .set_timestamp_us(timestamp_us)
                   .build();
  source->capturer->PushFrame(frame);
  return 1;
}

int lwrtc_video_source_push_argb(
    lwrtc_video_source_t* source,
    const uint8_t* data,
    int stride,
    int width,
    int height,
    int64_t timestamp_us) {
  if (!source || !source->capturer || !data || width <= 0 || height <= 0) {
    return 0;
  }

  auto buffer = webrtc::I420Buffer::Create(width, height);
  if (!buffer) {
    return 0;
  }

  libyuv::ConvertToI420(
      data, 0, buffer->MutableDataY(), buffer->StrideY(),
      buffer->MutableDataU(), buffer->StrideU(), buffer->MutableDataV(),
      buffer->StrideV(), 0, 0, width, height, width, height, libyuv::kRotate0,
      libyuv::FOURCC_ARGB);

  auto frame = webrtc::VideoFrame::Builder()
                   .set_video_frame_buffer(buffer)
                   .set_rotation(webrtc::kVideoRotation_0)
                   .set_timestamp_us(timestamp_us)
                   .build();
  source->capturer->PushFrame(frame);
  return 1;
}

int lwrtc_video_source_push_nv12(
    lwrtc_video_source_t* source,
    const uint8_t* data_y,
    int stride_y,
    const uint8_t* data_uv,
    int stride_uv,
    int width,
    int height,
    int64_t timestamp_us) {
  if (!source || !source->capturer || !data_y || !data_uv || width <= 0 ||
      height <= 0) {
    return 0;
  }

  auto buffer = webrtc::NV12Buffer::Create(width, height, stride_y, stride_uv);
  if (!buffer) {
    return 0;
  }

  libyuv::CopyPlane(
      data_y, stride_y, buffer->MutableDataY(), buffer->StrideY(), width,
      height);
  const int uv_height = (height + 1) / 2;
  libyuv::CopyPlane(
      data_uv, stride_uv, buffer->MutableDataUV(), buffer->StrideUV(), width,
      uv_height);

  auto frame = webrtc::VideoFrame::Builder()
                   .set_video_frame_buffer(buffer)
                   .set_rotation(webrtc::kVideoRotation_0)
                   .set_timestamp_us(timestamp_us)
                   .build();
  source->capturer->PushFrame(frame);
  return 1;
}

#ifdef _WIN32
int lwrtc_video_source_push_d3d11(
    lwrtc_video_source_t* source,
    void* texture,
    void* keyed_mutex,
    int width,
    int height,
    int64_t timestamp_us) {
  if (!source || !source->capturer || !texture || width <= 0 || height <= 0) {
    return 0;
  }

  auto buffer = owt::base::D3D11TextureBuffer::Create(
      static_cast<ID3D11Texture2D*>(texture),
      static_cast<IDXGIKeyedMutex*>(keyed_mutex),
      width,
      height);
  if (!buffer) {
    return 0;
  }

  auto frame = webrtc::VideoFrame::Builder()
                   .set_video_frame_buffer(buffer)
                   .set_rotation(webrtc::kVideoRotation_0)
                   .set_timestamp_us(timestamp_us)
                   .build();
  source->capturer->PushFrame(frame);
  return 1;
}
#else
int lwrtc_video_source_push_d3d11(
    lwrtc_video_source_t*,
    void*,
    void*,
    int,
    int,
    int64_t) {
  return 0;
}
#endif

lwrtc_audio_track_t* lwrtc_audio_track_create(
    lwrtc_factory_t* factory,
    lwrtc_audio_source_t* source,
    const char* track_id) {
  if (!factory || !factory->handle || !source || !source->source) {
    return nullptr;
  }
  const char* id = (track_id && *track_id) ? track_id : "audio";
  auto track = factory->handle->CreateAudioTrack(source->source, id);
  if (!track) {
    return nullptr;
  }
  auto* handle = new (std::nothrow) lwrtc_audio_track();
  if (!handle) {
    return nullptr;
  }
  handle->track = track;
  return handle;
}

lwrtc_video_track_t* lwrtc_video_track_create(
    lwrtc_factory_t* factory,
    lwrtc_video_source_t* source,
    const char* track_id) {
  if (!factory || !factory->handle || !source || !source->source) {
    return nullptr;
  }
  const char* id = (track_id && *track_id) ? track_id : "video";
  auto track = factory->handle->CreateVideoTrack(source->source, id);
  if (!track) {
    return nullptr;
  }
  auto* handle = new (std::nothrow) lwrtc_video_track();
  if (!handle) {
    return nullptr;
  }
  handle->track = track;
  return handle;
}

void lwrtc_audio_track_release(lwrtc_audio_track_t* track) {
  delete track;
}

void lwrtc_video_track_release(lwrtc_video_track_t* track) {
  delete track;
}

int lwrtc_peer_add_audio_track(
    lwrtc_peer_t* peer,
    lwrtc_audio_track_t* track,
    const char* stream_id) {
  if (!peer || !peer->handle || !track || !track->track) {
    return 0;
  }
  std::vector<libwebrtc::string> std_stream_ids;
  if (stream_id && *stream_id) {
    std_stream_ids.emplace_back(stream_id);
  } else {
    std_stream_ids.emplace_back("sunshine");
  }
  libwebrtc::vector<libwebrtc::string> stream_ids(std_stream_ids);
  auto sender = peer->handle->AddTrack(track->track, stream_ids);
  return sender ? 1 : 0;
}

int lwrtc_peer_add_video_track(
    lwrtc_peer_t* peer,
    lwrtc_video_track_t* track,
    const char* stream_id) {
  if (!peer || !peer->handle || !track || !track->track) {
    return 0;
  }
  std::vector<libwebrtc::string> std_stream_ids;
  if (stream_id && *stream_id) {
    std_stream_ids.emplace_back(stream_id);
  } else {
    std_stream_ids.emplace_back("sunshine");
  }
  libwebrtc::vector<libwebrtc::string> stream_ids(std_stream_ids);
  auto sender = peer->handle->AddTrack(track->track, stream_ids);
  return sender ? 1 : 0;
}

void lwrtc_data_channel_register_observer(
    lwrtc_data_channel_t* channel,
    lwrtc_data_channel_state_cb on_state,
    lwrtc_data_channel_message_cb on_message,
    void* user) {
  if (!channel || !channel->handle) {
    return;
  }
  channel->observer = std::make_unique<DataChannelObserver>(on_state, on_message, user);
  channel->handle->RegisterObserver(channel->observer.get());
}

void lwrtc_data_channel_unregister_observer(lwrtc_data_channel_t* channel) {
  if (!channel || !channel->handle) {
    return;
  }
  channel->handle->UnregisterObserver();
  channel->observer.reset();
}

const char* lwrtc_data_channel_label(lwrtc_data_channel_t* channel) {
  if (!channel) {
    return nullptr;
  }
  return channel->label.c_str();
}

int lwrtc_data_channel_state(lwrtc_data_channel_t* channel) {
  if (!channel || !channel->handle) {
    return LWRTC_DATA_CHANNEL_CLOSED;
  }
  return static_cast<int>(channel->handle->state());
}

int lwrtc_data_channel_send(
    lwrtc_data_channel_t* channel,
    const uint8_t* data,
    size_t size,
    int binary) {
  if (!channel || !channel->handle || !data || size == 0) {
    return 0;
  }
  channel->handle->Send(data, static_cast<uint32_t>(size), binary != 0);
  return 1;
}

void lwrtc_data_channel_close(lwrtc_data_channel_t* channel) {
  if (!channel || !channel->handle) {
    return;
  }
  channel->handle->Close();
}

void lwrtc_data_channel_release(lwrtc_data_channel_t* channel) {
  delete channel;
}

// Encoded video source implementation

lwrtc_encoded_video_source_t* lwrtc_encoded_video_source_create(
    lwrtc_factory_t* factory,
    lwrtc_video_codec_t codec,
    int width,
    int height) {
  if (!factory || !factory->handle) {
    return nullptr;
  }

  if (!factory->use_passthrough) {
    // Passthrough mode must be enabled via lwrtc_factory_enable_passthrough()
    // before lwrtc_factory_initialize()
    return nullptr;
  }

  auto* source = new (std::nothrow) lwrtc_encoded_video_source();
  if (!source) {
    return nullptr;
  }

  source->factory = factory;
  source->codec = codec;
  source->width = width;
  source->height = height;

  return source;
}

void lwrtc_encoded_video_source_release(lwrtc_encoded_video_source_t* source) {
  delete source;
}

namespace {
constexpr int64_t kDummyPushIntervalUs = 200000;

class ExternalEncodedImageBuffer : public webrtc::EncodedImageBufferInterface {
 public:
  ExternalEncodedImageBuffer(const uint8_t* data,
                             size_t size,
                             lwrtc_buffer_release_cb release_cb,
                             void* release_user)
      : data_(data),
        size_(size),
        release_cb_(release_cb),
        release_user_(release_user) {}

  const uint8_t* data() const override { return data_; }
  uint8_t* data() override { return const_cast<uint8_t*>(data_); }
  size_t size() const override { return size_; }

 private:
  friend class rtc::RefCountedObject<ExternalEncodedImageBuffer>;
  ~ExternalEncodedImageBuffer() override {
    if (release_cb_) {
      release_cb_(release_user_);
    }
  }

  const uint8_t* const data_;
  const size_t size_;
  const lwrtc_buffer_release_cb release_cb_;
  void* const release_user_;
};

void PushDummyFrameIfNeeded(lwrtc_encoded_video_source_t* source,
                            int64_t timestamp_us) {
  if (!source || !source->capturer) {
    return;
  }
  if (source->last_dummy_push_us != 0 &&
      timestamp_us - source->last_dummy_push_us < kDummyPushIntervalUs) {
    return;
  }
  if (source->width <= 0 || source->height <= 0) {
    return;
  }
  if (!source->dummy_buffer || source->dummy_width != source->width ||
      source->dummy_height != source->height) {
    source->dummy_buffer = webrtc::I420Buffer::Create(source->width, source->height);
    source->dummy_width = source->width;
    source->dummy_height = source->height;
    memset(source->dummy_buffer->MutableDataY(), 0,
           source->dummy_buffer->StrideY() * source->height);
    memset(source->dummy_buffer->MutableDataU(), 128,
           source->dummy_buffer->StrideU() * ((source->height + 1) / 2));
    memset(source->dummy_buffer->MutableDataV(), 128,
           source->dummy_buffer->StrideV() * ((source->height + 1) / 2));
  }

  webrtc::VideoFrame video_frame = webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(source->dummy_buffer)
      .set_timestamp_us(timestamp_us)
      .set_rotation(webrtc::kVideoRotation_0)
      .build();
  source->capturer->PushFrame(video_frame);
  source->last_dummy_push_us = timestamp_us;
}
}  // namespace

int lwrtc_encoded_video_source_push(
    lwrtc_encoded_video_source_t* source,
    const uint8_t* data,
    size_t size,
    int64_t timestamp_us,
    int is_keyframe) {
  if (!source || !data || size == 0) {
    return 0;
  }

  if (!source->factory || !source->factory->handle) {
    return 0;
  }

  auto* passthrough_factory = source->factory->passthrough_factory;
  if (!passthrough_factory) {
    return 0;
  }
  auto* encoder = passthrough_factory->GetActiveEncoder();
  if (!encoder) {
    // Encoder not yet created by WebRTC - push a dummy frame through the
    // capturer to trigger encoder creation
    if (source->capturer && !source->encoder_ready) {
      // Create a black I420 frame to trigger the encoding pipeline
      auto i420_buffer = webrtc::I420Buffer::Create(source->width, source->height);
      // Fill with black (Y=0, U=128, V=128)
      memset(i420_buffer->MutableDataY(), 0,
             i420_buffer->StrideY() * source->height);
      memset(i420_buffer->MutableDataU(), 128,
             i420_buffer->StrideU() * ((source->height + 1) / 2));
      memset(i420_buffer->MutableDataV(), 128,
             i420_buffer->StrideV() * ((source->height + 1) / 2));

      webrtc::VideoFrame video_frame = webrtc::VideoFrame::Builder()
          .set_video_frame_buffer(i420_buffer)
          .set_timestamp_us(timestamp_us)
          .set_rotation(webrtc::kVideoRotation_0)
          .build();

      source->capturer->PushFrame(video_frame);
    }
    return 0;  // Frame not pushed to encoder yet, will be discarded
  }

  // Encoder is ready - mark it and stop pushing dummy frames
  source->encoder_ready = true;
  PushDummyFrameIfNeeded(source, timestamp_us);

  owt::base::EncodedFrameData frame;
  frame.buffer = webrtc::EncodedImageBuffer::Create(data, size);
  frame.width = source->width;
  frame.height = source->height;
  frame.timestamp_us = timestamp_us;
  frame.is_keyframe = is_keyframe != 0;
  frame.codec_type = ToWebrtcCodec(source->codec);

  return encoder->PushEncodedFrame(frame) ? 1 : 0;
}

int lwrtc_encoded_video_source_push_shared(
    lwrtc_encoded_video_source_t* source,
    const uint8_t* data,
    size_t size,
    int64_t timestamp_us,
    int is_keyframe,
    lwrtc_buffer_release_cb release_cb,
    void* release_user) {
  if (!source || !data || size == 0) {
    if (release_cb) {
      release_cb(release_user);
    }
    return 0;
  }

  if (!source->factory || !source->factory->handle) {
    if (release_cb) {
      release_cb(release_user);
    }
    return 0;
  }

  auto* passthrough_factory = source->factory->passthrough_factory;
  if (!passthrough_factory) {
    if (release_cb) {
      release_cb(release_user);
    }
    return 0;
  }
  auto* encoder = passthrough_factory->GetActiveEncoder();
  if (!encoder) {
    // Encoder not yet created by WebRTC - push a dummy frame through the
    // capturer to trigger encoder creation
    if (source->capturer && !source->encoder_ready) {
      // Create a black I420 frame to trigger the encoding pipeline
      auto i420_buffer = webrtc::I420Buffer::Create(source->width, source->height);
      // Fill with black (Y=0, U=128, V=128)
      memset(i420_buffer->MutableDataY(), 0,
             i420_buffer->StrideY() * source->height);
      memset(i420_buffer->MutableDataU(), 128,
             i420_buffer->StrideU() * ((source->height + 1) / 2));
      memset(i420_buffer->MutableDataV(), 128,
             i420_buffer->StrideV() * ((source->height + 1) / 2));

      webrtc::VideoFrame video_frame = webrtc::VideoFrame::Builder()
          .set_video_frame_buffer(i420_buffer)
          .set_timestamp_us(timestamp_us)
          .set_rotation(webrtc::kVideoRotation_0)
          .build();

      source->capturer->PushFrame(video_frame);
    }
    if (release_cb) {
      release_cb(release_user);
    }
    return 0;  // Frame not pushed to encoder yet, will be discarded
  }

  // Encoder is ready - mark it and stop pushing dummy frames
  source->encoder_ready = true;
  PushDummyFrameIfNeeded(source, timestamp_us);

  owt::base::EncodedFrameData frame;
  frame.buffer = new rtc::RefCountedObject<ExternalEncodedImageBuffer>(
      data, size, release_cb, release_user);
  frame.width = source->width;
  frame.height = source->height;
  frame.timestamp_us = timestamp_us;
  frame.is_keyframe = is_keyframe != 0;
  frame.codec_type = ToWebrtcCodec(source->codec);

  return encoder->PushEncodedFrame(frame) ? 1 : 0;
}

void lwrtc_encoded_video_source_set_keyframe_callback(
    lwrtc_encoded_video_source_t* source,
    lwrtc_keyframe_request_cb cb,
    void* user) {
  if (!source) {
    return;
  }

  source->keyframe_cb = cb;
  source->keyframe_user = user;

  // If factory exists, set the callback on the passthrough encoder factory
  if (source->factory && source->factory->handle) {
    auto* passthrough_factory = source->factory->passthrough_factory;
    if (!passthrough_factory) {
      return;
    }
    auto cb_copy = source->keyframe_cb;
    auto user_copy = source->keyframe_user;
    passthrough_factory->SetKeyframeRequestCallback([cb_copy, user_copy]() {
      if (cb_copy) {
        cb_copy(user_copy);
      }
    });
  }
}

lwrtc_video_track_t* lwrtc_encoded_video_track_create(
    lwrtc_factory_t* factory,
    lwrtc_encoded_video_source_t* source,
    const char* track_id) {
  if (!factory || !factory->handle || !source) {
    return nullptr;
  }

  // For passthrough mode, we still need a video source/track for WebRTC.
  // We create a capturer that will be used to trigger encoder creation.
  // Once the encoder is ready, actual encoded frames bypass this capturer.
  auto capturer = std::make_shared<PushVideoCapturer>();
  source->capturer = capturer;  // Store for later use
  auto rtc_track_source = webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>(
      new rtc::RefCountedObject<webrtc::internal::CapturerTrackSource>(capturer));
  auto video_source =
      libwebrtc::scoped_refptr<libwebrtc::RTCVideoSourceImpl>(
          new libwebrtc::RefCountedObject<libwebrtc::RTCVideoSourceImpl>(
              rtc_track_source));

  const char* id = (track_id && *track_id) ? track_id : "video";
  auto track = factory->handle->CreateVideoTrack(video_source, id);
  if (!track) {
    return nullptr;
  }

  auto* handle = new (std::nothrow) lwrtc_video_track();
  if (!handle) {
    return nullptr;
  }
  handle->track = track;

  // Set up keyframe callback if one was registered
  if (source->keyframe_cb) {
    auto* passthrough_factory = factory->passthrough_factory;
    if (passthrough_factory) {
      auto cb_copy = source->keyframe_cb;
      auto user_copy = source->keyframe_user;
      passthrough_factory->SetKeyframeRequestCallback([cb_copy, user_copy]() {
        if (cb_copy) {
          cb_copy(user_copy);
        }
      });
    }
  }

  return handle;
}

}  // extern "C"
