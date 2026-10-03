#ifndef LIB_WEBRTC_MEDIA_SESSION_FACTORY_IMPL_HXX
#define LIB_WEBRTC_MEDIA_SESSION_FACTORY_IMPL_HXX

#include <cstdint>
#include <memory>

#include "api/audio_options.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/task_queue/task_queue_factory.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "modules/audio_device/include/audio_device.h"
#include "rtc_audio_device_impl.h"
#include "rtc_audio_processing_impl.h"
#include "rtc_base/thread.h"
#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"
#include "rtc_video_device_impl.h"

#ifdef RTC_DESKTOP_DEVICE
#include "rtc_desktop_capturer_impl.h"
#include "rtc_desktop_device_impl.h"
#include "src/internal/desktop_capturer.h"
#endif

namespace libwebrtc {

class RTCPeerConnectionFactoryImpl : public RTCPeerConnectionFactory {
 public:
  RTCPeerConnectionFactoryImpl();

  virtual ~RTCPeerConnectionFactoryImpl();

  bool Initialize() override;

  bool Terminate() override;

  scoped_refptr<RTCPeerConnection> Create(
      const RTCConfiguration& configuration,
      scoped_refptr<RTCMediaConstraints> constraints) override;

  void Delete(scoped_refptr<RTCPeerConnection> peerconnection) override;

  scoped_refptr<RTCAudioDevice> GetAudioDevice() override;

  scoped_refptr<RTCVideoDevice> GetVideoDevice() override;

  scoped_refptr<RTCAudioProcessing> GetAudioProcessing() override;

  virtual scoped_refptr<RTCAudioSource> CreateAudioSource(
      const string audio_source_label,
      RTCAudioSource::SourceType source_type) override;

  virtual scoped_refptr<RTCVideoSource> CreateVideoSource(
      scoped_refptr<RTCVideoCapturer> capturer, const string video_source_label,
      scoped_refptr<RTCMediaConstraints> constraints) override;
#ifdef RTC_DESKTOP_DEVICE
  virtual scoped_refptr<RTCDesktopDevice> GetDesktopDevice() override;
  virtual scoped_refptr<RTCVideoSource> CreateDesktopSource(
      scoped_refptr<RTCDesktopCapturer> capturer,
      const string video_source_label,
      scoped_refptr<RTCMediaConstraints> constraints) override;
#endif
  virtual scoped_refptr<RTCAudioTrack> CreateAudioTrack(
      scoped_refptr<RTCAudioSource> source, const string track_id) override;

  virtual scoped_refptr<RTCVideoTrack> CreateVideoTrack(
      scoped_refptr<RTCVideoSource> source, const string track_id) override;

  virtual scoped_refptr<RTCMediaStream> CreateStream(
      const string stream_id) override;

  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
  peer_connection_factory() {
    return rtc_peerconnection_factory_;
  }

  scoped_refptr<RTCRtpCapabilities> GetRtpSenderCapabilities(
      RTCMediaType media_type) override;

  scoped_refptr<RTCRtpCapabilities> GetRtpReceiverCapabilities(
      RTCMediaType media_type) override;

  rtc::Thread* signaling_thread() { return signaling_thread_.get(); }

  // Set a custom video encoder factory.
  // Must be called before Initialize().
  // The factory takes ownership of the encoder factory.
  void SetVideoEncoderFactory(
      std::unique_ptr<webrtc::VideoEncoderFactory> factory);

  // Get the custom video encoder factory (if set).
  webrtc::VideoEncoderFactory* GetVideoEncoderFactory();

  // Builds the factory over webrtc's dummy audio device instead of the
  // platform one (Windows Core Audio), for a caller that plays nothing and
  // pushes its own captured audio through a kCustom audio source. The dummy
  // device opens no audio endpoint, at Initialize() or when an audio stream
  // is added. Must be called before Initialize().
  void UseDummyAudioDevice();

  // How long each step of Initialize() took, in microseconds. All zero until
  // Initialize() has run; a step Initialize() skipped stays zero.
  struct InitializeTimings {
    int64_t threads_us = 0;
    int64_t audio_device_create_us = 0;
    int64_t audio_device_init_us = 0;
    int64_t audio_processing_us = 0;
    int64_t peer_connection_factory_us = 0;
  };

  const InitializeTimings& initialize_timings() const {
    return initialize_timings_;
  }

 protected:
  void CreateAudioDeviceModule_w();

  void DestroyAudioDeviceModule_w();

  webrtc::scoped_refptr<webrtc::AudioSourceInterface>
  CreateAudioSourceWithOptions(const cricket::AudioOptions* options,
                               bool is_custom_source = false);

  scoped_refptr<RTCVideoSource> CreateVideoSource_s(
      scoped_refptr<RTCVideoCapturer> capturer, const char* video_source_label,
      scoped_refptr<RTCMediaConstraints> constraints);
#ifdef RTC_DESKTOP_DEVICE
  scoped_refptr<RTCVideoSource> CreateDesktopSource_d(
      scoped_refptr<RTCDesktopCapturer> capturer,
      const char* video_source_label,
      scoped_refptr<RTCMediaConstraints> constraints);
#endif
 private:
  std::unique_ptr<rtc::Thread> worker_thread_;
  std::unique_ptr<rtc::Thread> signaling_thread_;
  std::unique_ptr<rtc::Thread> network_thread_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
      rtc_peerconnection_factory_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_module_;
  scoped_refptr<AudioDeviceImpl> audio_device_impl_;
  scoped_refptr<RTCAudioProcessingImpl> audio_processing_impl_;
  scoped_refptr<RTCVideoDeviceImpl> video_device_impl_;
#ifdef RTC_DESKTOP_DEVICE
  scoped_refptr<RTCDesktopDeviceImpl> desktop_device_impl_;
#endif
  std::list<scoped_refptr<RTCPeerConnection>> peerconnections_;
  std::unique_ptr<webrtc::TaskQueueFactory> task_queue_factory_;
  std::unique_ptr<webrtc::VideoEncoderFactory> custom_encoder_factory_;
  webrtc::VideoEncoderFactory* custom_encoder_factory_ptr_ = nullptr;  // Non-owning pointer
  webrtc::AudioDeviceModule::AudioLayer audio_layer_ =
      webrtc::AudioDeviceModule::kPlatformDefaultAudio;
  InitializeTimings initialize_timings_;
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_MEDIA_SESSION_FACTORY_IMPL_HXX

