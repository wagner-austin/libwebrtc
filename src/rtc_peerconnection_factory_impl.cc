#include "rtc_peerconnection_factory_impl.h"

#include "internal/media_type_utils.h"

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/media_stream_interface.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "modules/audio_device/audio_device_impl.h"
#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/time_utils.h"
#include "rtc_audio_source_impl.h"
#include "rtc_media_stream_impl.h"
#include "rtc_mediaconstraints_impl.h"
#include "rtc_peerconnection_impl.h"
#include "rtc_rtp_capabilities_impl.h"
#include "rtc_video_device_impl.h"
#include "rtc_video_source_impl.h"
#include "src/internal/local_audio_track.h"
#if defined(USE_INTEL_MEDIA_SDK)
#include "src/win/mediacapabilities.h"
#include "src/win/msdkvideodecoderfactory.h"
#include "src/win/msdkvideoencoderfactory.h"
#endif
#if defined(WEBRTC_IOS)
#include "engine/sdk/objc/Framework/Classes/videotoolboxvideocodecfactory.h"
#endif
#include <api/task_queue/default_task_queue_factory.h>

namespace libwebrtc {

#if defined(USE_INTEL_MEDIA_SDK)
std::unique_ptr<webrtc::VideoEncoderFactory> CreateIntelVideoEncoderFactory() {
  if (!owt::base::MediaCapabilities::Get()) {
    return webrtc::CreateBuiltinVideoEncoderFactory();
  }
  return std::make_unique<owt::base::MSDKVideoEncoderFactory>();
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreateIntelVideoDecoderFactory() {
  if (!owt::base::MediaCapabilities::Get()) {
    return webrtc::CreateBuiltinVideoDecoderFactory();
  }
  return std::make_unique<owt::base::MSDKVideoDecoderFactory>();
}
#endif

RTCPeerConnectionFactoryImpl::RTCPeerConnectionFactoryImpl() {}

RTCPeerConnectionFactoryImpl::~RTCPeerConnectionFactoryImpl() {}

bool RTCPeerConnectionFactoryImpl::Initialize() {
  initialize_timings_ = InitializeTimings {};
  const int64_t threads_start_us = rtc::TimeMicros();
  worker_thread_ = rtc::Thread::Create();
  worker_thread_->SetName("worker_thread", nullptr);
  RTC_CHECK(worker_thread_->Start()) << "Failed to start thread";

  signaling_thread_ = rtc::Thread::Create();
  signaling_thread_->SetName("signaling_thread", nullptr);
  RTC_CHECK(signaling_thread_->Start()) << "Failed to start thread";

  network_thread_ = rtc::Thread::CreateWithSocketServer();
  network_thread_->SetName("network_thread", nullptr);
  RTC_CHECK(network_thread_->Start()) << "Failed to start thread";
  initialize_timings_.threads_us = rtc::TimeMicros() - threads_start_us;
  if (!audio_device_module_) {
    task_queue_factory_ = webrtc::CreateDefaultTaskQueueFactory();
    const int64_t create_start_us = rtc::TimeMicros();
    worker_thread_->BlockingCall([&] { CreateAudioDeviceModule_w(); });
    initialize_timings_.audio_device_create_us =
        rtc::TimeMicros() - create_start_us;
    if (!audio_device_module_) {
      RTC_LOG(LS_ERROR) << "Failed to create the platform audio device module";
      Terminate();
      return false;
    }
    // The media engine initializes the device again inside
    // CreatePeerConnectionFactory, where a failure is an RTC_CHECK; done here
    // first, its cost is measured on its own and a failure returns false.
    // A second Init() on an initialized device returns 0 at once.
    const int64_t init_start_us = rtc::TimeMicros();
    const int32_t init_result = worker_thread_->BlockingCall(
        [this] { return audio_device_module_->Init(); });
    initialize_timings_.audio_device_init_us =
        rtc::TimeMicros() - init_start_us;
    if (init_result != 0) {
      RTC_LOG(LS_ERROR) << "Failed to initialize the platform audio device "
                        << "module: " << init_result;
      Terminate();
      return false;
    }
  }

  if (!audio_processing_impl_) {
    const int64_t processing_start_us = rtc::TimeMicros();
    worker_thread_->BlockingCall([this] {
      audio_processing_impl_ = new RefCountedObject<RTCAudioProcessingImpl>();
    });
    initialize_timings_.audio_processing_us =
        rtc::TimeMicros() - processing_start_us;
  }

  if (!rtc_peerconnection_factory_) {
    const int64_t factory_start_us = rtc::TimeMicros();
    // Determine which video encoder factory to use
    std::unique_ptr<webrtc::VideoEncoderFactory> encoder_factory;
    if (custom_encoder_factory_) {
      // Store raw pointer before the move so we can access it later
      custom_encoder_factory_ptr_ = custom_encoder_factory_.get();
      encoder_factory = std::move(custom_encoder_factory_);
    } else {
#if defined(USE_INTEL_MEDIA_SDK)
      encoder_factory = CreateIntelVideoEncoderFactory();
#else
      encoder_factory = webrtc::CreateBuiltinVideoEncoderFactory();
#endif
    }

    // Determine which video decoder factory to use
    std::unique_ptr<webrtc::VideoDecoderFactory> decoder_factory;
    if (custom_decoder_factory_) {
      decoder_factory = std::move(custom_decoder_factory_);
    } else {
#if defined(USE_INTEL_MEDIA_SDK)
      decoder_factory = CreateIntelVideoDecoderFactory();
#else
      decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();
#endif
    }

    rtc_peerconnection_factory_ = CreatePeerConnectionFactory(
        network_thread_.get(), worker_thread_.get(), signaling_thread_.get(),
        audio_device_module_, webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        std::move(encoder_factory), std::move(decoder_factory),
        nullptr, audio_processing_impl_->GetAudioProcessing(), nullptr, nullptr);
    initialize_timings_.peer_connection_factory_us =
        rtc::TimeMicros() - factory_start_us;
  }

  if (!rtc_peerconnection_factory_.get()) {
    Terminate();
    return false;
  }

  return true;
}

bool RTCPeerConnectionFactoryImpl::Terminate() {
  worker_thread_->BlockingCall([&] {
    audio_device_impl_ = nullptr;
    video_device_impl_ = nullptr;
    audio_processing_impl_ = nullptr;
  });
  rtc_peerconnection_factory_ = NULL;
  if (audio_device_module_) {
    worker_thread_->BlockingCall([this] { DestroyAudioDeviceModule_w(); });
  }

  return true;
}

void RTCPeerConnectionFactoryImpl::CreateAudioDeviceModule_w() {
  if (!audio_device_module_)
    audio_device_module_ = webrtc::AudioDeviceModule::Create(
        webrtc::AudioDeviceModule::kPlatformDefaultAudio,
        task_queue_factory_.get());
}

void RTCPeerConnectionFactoryImpl::DestroyAudioDeviceModule_w() {
  if (audio_device_module_) audio_device_module_ = nullptr;
}

scoped_refptr<RTCPeerConnection> RTCPeerConnectionFactoryImpl::Create(
    const RTCConfiguration& configuration,
    scoped_refptr<RTCMediaConstraints> constraints) {
  scoped_refptr<RTCPeerConnection> peerconnection;
  if (rtc::Thread::Current() != signaling_thread_.get()) {
    peerconnection = signaling_thread_->BlockingCall([this, &configuration, constraints] {
      return scoped_refptr<RTCPeerConnectionImpl>(
          new RefCountedObject<RTCPeerConnectionImpl>(
              configuration, constraints, rtc_peerconnection_factory_));
    });
  } else {
    peerconnection = scoped_refptr<RTCPeerConnectionImpl>(
        new RefCountedObject<RTCPeerConnectionImpl>(
            configuration, constraints, rtc_peerconnection_factory_));
  }
  peerconnections_.push_back(peerconnection);
  return peerconnection;
}

void RTCPeerConnectionFactoryImpl::Delete(
    scoped_refptr<RTCPeerConnection> peerconnection) {
  peerconnections_.erase(
      std::remove_if(
          peerconnections_.begin(), peerconnections_.end(),
          [peerconnection](const scoped_refptr<RTCPeerConnection> pc_) {
            return pc_ == peerconnection;
          }),
      peerconnections_.end());
}

scoped_refptr<RTCAudioDevice> RTCPeerConnectionFactoryImpl::GetAudioDevice() {
  if (!audio_device_module_) {
    worker_thread_->BlockingCall([this] { CreateAudioDeviceModule_w(); });
  }

  if (!audio_device_impl_)
    audio_device_impl_ =
        scoped_refptr<AudioDeviceImpl>(new RefCountedObject<AudioDeviceImpl>(
            audio_device_module_, worker_thread_.get()));

  return audio_device_impl_;
}

scoped_refptr<RTCAudioProcessing>
RTCPeerConnectionFactoryImpl::GetAudioProcessing() {
  if (!audio_processing_impl_) {
    worker_thread_->BlockingCall([this] {
      audio_processing_impl_ = new RefCountedObject<RTCAudioProcessingImpl>();
    });
  }

  return audio_processing_impl_;
}

scoped_refptr<RTCVideoDevice> RTCPeerConnectionFactoryImpl::GetVideoDevice() {
  if (!video_device_impl_)
    video_device_impl_ = scoped_refptr<RTCVideoDeviceImpl>(
        new RefCountedObject<RTCVideoDeviceImpl>(worker_thread_.get()));

  return video_device_impl_;
}

webrtc::scoped_refptr<webrtc::AudioSourceInterface>
RTCPeerConnectionFactoryImpl::CreateAudioSourceWithOptions(
    const cricket::AudioOptions* options, bool is_custom_source) {
  RTC_DCHECK(options);
  // For custom sources, we rely on LocalAudioSource::OnData to push frames.
  if (rtc::Thread::Current() != signaling_thread_.get()) {
    return signaling_thread_->BlockingCall([this, options, is_custom_source] {
      if (is_custom_source) {
        return webrtc::scoped_refptr<webrtc::AudioSourceInterface>(
            libwebrtc::LocalAudioSource::Create(options, nullptr));
      }
      return rtc_peerconnection_factory_->CreateAudioSource(*options);
    });
  }
  if (is_custom_source) {
    return webrtc::scoped_refptr<webrtc::AudioSourceInterface>(
        libwebrtc::LocalAudioSource::Create(options, nullptr));
  }
  return rtc_peerconnection_factory_->CreateAudioSource(*options);
}

scoped_refptr<RTCAudioSource> RTCPeerConnectionFactoryImpl::CreateAudioSource(
    const string audio_source_label, RTCAudioSource::SourceType source_type) {
  auto options = cricket::AudioOptions();
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> rtc_source_track =
      CreateAudioSourceWithOptions(&options, source_type == RTCAudioSource::kCustom);
  scoped_refptr<RTCAudioSourceImpl> source = scoped_refptr<RTCAudioSourceImpl>(
      new RefCountedObject<RTCAudioSourceImpl>(rtc_source_track, source_type));
  return source;
}

#ifdef RTC_DESKTOP_DEVICE
scoped_refptr<RTCDesktopDevice>
RTCPeerConnectionFactoryImpl::GetDesktopDevice() {
  if (!desktop_device_impl_) {
    desktop_device_impl_ = scoped_refptr<RTCDesktopDeviceImpl>(
        new RefCountedObject<RTCDesktopDeviceImpl>(signaling_thread_.get()));
  }
  return desktop_device_impl_;
}
#endif

scoped_refptr<RTCVideoSource> RTCPeerConnectionFactoryImpl::CreateVideoSource(
    scoped_refptr<RTCVideoCapturer> capturer, const string video_source_label,
    scoped_refptr<RTCMediaConstraints> constraints) {
  if (rtc::Thread::Current() != signaling_thread_.get()) {
    scoped_refptr<RTCVideoSource> source = signaling_thread_->BlockingCall(
        [this, capturer, video_source_label, constraints] {
          return CreateVideoSource_s(
              capturer, to_std_string(video_source_label).c_str(), constraints);
        });
    return source;
  }

  return CreateVideoSource_s(
      capturer, to_std_string(video_source_label).c_str(), constraints);
}

scoped_refptr<RTCVideoSource> RTCPeerConnectionFactoryImpl::CreateVideoSource_s(
    scoped_refptr<RTCVideoCapturer> capturer, const char* video_source_label,
    scoped_refptr<RTCMediaConstraints> constraints) {
  RTCVideoCapturerImpl* capturer_impl =
      static_cast<RTCVideoCapturerImpl*>(capturer.get());
  /*RTCMediaConstraintsImpl* media_constraints =
          static_cast<RTCMediaConstraintsImpl*>(constraints.get());*/
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> rtc_source_track =
      webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>(
          new rtc::RefCountedObject<webrtc::internal::CapturerTrackSource>(
              capturer_impl->video_capturer()));
  scoped_refptr<RTCVideoSourceImpl> source = scoped_refptr<RTCVideoSourceImpl>(
      new RefCountedObject<RTCVideoSourceImpl>(rtc_source_track));
  return source;
}

#ifdef RTC_DESKTOP_DEVICE
scoped_refptr<RTCVideoSource> RTCPeerConnectionFactoryImpl::CreateDesktopSource(
    scoped_refptr<RTCDesktopCapturer> capturer, const string video_source_label,
    scoped_refptr<RTCMediaConstraints> constraints) {
  if (rtc::Thread::Current() != signaling_thread_.get()) {
    scoped_refptr<RTCVideoSource> source = signaling_thread_->BlockingCall(
        [this, capturer, video_source_label, constraints] {
          return CreateDesktopSource_d(
              capturer, to_std_string(video_source_label).c_str(), constraints);
        });
    return source;
  }

  return CreateDesktopSource_d(
      capturer, to_std_string(video_source_label).c_str(), constraints);
}

scoped_refptr<RTCVideoSource>
RTCPeerConnectionFactoryImpl::CreateDesktopSource_d(
    scoped_refptr<RTCDesktopCapturer> capturer, const char* video_source_label,
    scoped_refptr<RTCMediaConstraints> constraints) {
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> rtc_source_track =
      webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>(
          new rtc::RefCountedObject<ScreenCapturerTrackSource>(capturer));

  scoped_refptr<RTCVideoSourceImpl> source = scoped_refptr<RTCVideoSourceImpl>(
      new RefCountedObject<RTCVideoSourceImpl>(rtc_source_track));

  return source;
}
#endif

scoped_refptr<RTCMediaStream> RTCPeerConnectionFactoryImpl::CreateStream(
    const string stream_id) {
  webrtc::scoped_refptr<webrtc::MediaStreamInterface> rtc_stream =
      rtc_peerconnection_factory_->CreateLocalMediaStream(
          to_std_string(stream_id));

  scoped_refptr<MediaStreamImpl> stream = scoped_refptr<MediaStreamImpl>(
      new RefCountedObject<MediaStreamImpl>(rtc_stream));

  return stream;
}

scoped_refptr<RTCVideoTrack> RTCPeerConnectionFactoryImpl::CreateVideoTrack(
    scoped_refptr<RTCVideoSource> source, const string track_id) {
  scoped_refptr<RTCVideoSourceImpl> source_adapter(
      static_cast<RTCVideoSourceImpl*>(source.get()));
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> rtc_video_track =
      rtc_peerconnection_factory_->CreateVideoTrack(
          source_adapter->rtc_source_track(), track_id.std_string());

  scoped_refptr<VideoTrackImpl> video_track = scoped_refptr<VideoTrackImpl>(
      new RefCountedObject<VideoTrackImpl>(rtc_video_track));

  // 	webrtc::VideoTrackProxyWithInternal<webrtc::VideoTrackInterface>
  // *track_proxy =
  // dynamic_cast<webrtc::VideoTrackProxyWithInternal<webrtc::VideoTrackInterface>
  // *>(video_track.get()); 	if (track_proxy) {
  // 		webrtc::MediaStreamTrack<VideoTrackInterface> *track =
  // dynamic_cast<webrtc::MediaStreamTrack<VideoTrackInterface>*>(track_proxy->internal());
  // 		LOG(INFO) << "VideoTrackInterface: " << track->id();
  // 	}

  return video_track;
}

scoped_refptr<RTCAudioTrack> RTCPeerConnectionFactoryImpl::CreateAudioTrack(
    scoped_refptr<RTCAudioSource> source, const string track_id) {
  RTCAudioSourceImpl* source_impl =
      static_cast<RTCAudioSourceImpl*>(source.get());

  webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(
      rtc_peerconnection_factory_->CreateAudioTrack(
          to_std_string(track_id), source_impl->rtc_audio_source().get()));

  scoped_refptr<AudioTrackImpl> track = scoped_refptr<AudioTrackImpl>(
      new RefCountedObject<AudioTrackImpl>(audio_track));
  return track;
}

scoped_refptr<RTCRtpCapabilities>
RTCPeerConnectionFactoryImpl::GetRtpSenderCapabilities(
    RTCMediaType media_type) {
  if (rtc::Thread::Current() != signaling_thread_.get()) {
    scoped_refptr<RTCRtpCapabilities> capabilities =
        signaling_thread_->BlockingCall([this, media_type] {
          return GetRtpSenderCapabilities(media_type);
        });
    return capabilities;
  }

  cricket::MediaType type = ToCricketMediaType(media_type);
  webrtc::RtpCapabilities rtp_capabilities =
      rtc_peerconnection_factory_->GetRtpSenderCapabilities(type);
  return scoped_refptr<RTCRtpCapabilities>(
      new RefCountedObject<RTCRtpCapabilitiesImpl>(rtp_capabilities));
}

scoped_refptr<RTCRtpCapabilities>
RTCPeerConnectionFactoryImpl::GetRtpReceiverCapabilities(
    RTCMediaType media_type) {
  if (rtc::Thread::Current() != signaling_thread_.get()) {
    scoped_refptr<RTCRtpCapabilities> capabilities =
        signaling_thread_->BlockingCall([this, media_type] {
          return GetRtpSenderCapabilities(media_type);
        });
    return capabilities;
  }
  cricket::MediaType type = ToCricketMediaType(media_type);
  webrtc::RtpCapabilities rtp_capabilities =
      rtc_peerconnection_factory_->GetRtpReceiverCapabilities(type);
  return scoped_refptr<RTCRtpCapabilities>(
      new RefCountedObject<RTCRtpCapabilitiesImpl>(rtp_capabilities));
}

void RTCPeerConnectionFactoryImpl::SetVideoEncoderFactory(
    std::unique_ptr<webrtc::VideoEncoderFactory> factory) {
  custom_encoder_factory_ = std::move(factory);
}

webrtc::VideoEncoderFactory* RTCPeerConnectionFactoryImpl::GetVideoEncoderFactory() {
  return custom_encoder_factory_ptr_;
}

void RTCPeerConnectionFactoryImpl::SetVideoDecoderFactory(
    std::unique_ptr<webrtc::VideoDecoderFactory> factory) {
  custom_decoder_factory_ = std::move(factory);
}

}  // namespace libwebrtc

