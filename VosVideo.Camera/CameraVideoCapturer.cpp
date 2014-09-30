#include "stdafx.h"
#include <boost/lexical_cast.hpp>
#include <modules/video_capture/include/video_capture_factory.h>
#include <modules/video_capture/include/video_capture_defines.h>
#include <talk/base/logging.h>
#include <vpx/vpx_encoder.h>
#include <talk/media/webrtc/webrtcvideocapturer.h>


#include "CameraVideoCapturer.h"
#include "CameraVideoCaptureImpl.h"

using namespace std;
using namespace cricket;
using namespace webrtc;

using boost::lexical_cast;
using boost::bad_lexical_cast;
using vosvideo::camera::CameraVcmFactoryInterface;
using vosvideo::camera::CameraVideoCapturer;
using vosvideo::camera::CameraVideoCaptureImpl;
using vosvideo::camera::CameraPlayer;


struct kVideoFourCCEntry 
{
	uint32 fourcc;
	webrtc::RawVideoType webrtc_type;
};

// This indicates our format preferences and defines a mapping between
// webrtc::RawVideoType (from video_capture_defines.h) to our FOURCCs.
static kVideoFourCCEntry kSupportedFourCCs[] = 
{
	{ FOURCC_I420, webrtc::kVideoI420 },   // 12 bpp, no conversion.
	{ FOURCC_YV12, webrtc::kVideoYV12 },   // 12 bpp, no conversion.
	{ FOURCC_NV12, webrtc::kVideoNV12 },   // 12 bpp, fast conversion.
	{ FOURCC_NV21, webrtc::kVideoNV21 },   // 12 bpp, fast conversion.
	{ FOURCC_YUY2, webrtc::kVideoYUY2 },   // 16 bpp, fast conversion.
	{ FOURCC_UYVY, webrtc::kVideoUYVY },   // 16 bpp, fast conversion.
	{ FOURCC_MJPG, webrtc::kVideoMJPEG },  // compressed, slow conversion.
	{ FOURCC_ARGB, webrtc::kVideoARGB },   // 32 bpp, slow conversion.
	{ FOURCC_24BG, webrtc::kVideoRGB24 },  // 24 bpp, slow conversion.
};


static bool CapabilityToFormat(const webrtc::VideoCaptureCapability& cap, VideoFormat* format) 
{
	uint32 fourcc = 0;
	for (size_t i = 0; i < ARRAY_SIZE(kSupportedFourCCs); ++i) 
	{
		if (kSupportedFourCCs[i].webrtc_type == cap.rawType) 
		{
			fourcc = kSupportedFourCCs[i].fourcc;
			break;
		}
	}
	if (fourcc == 0) 
	{
		return false;
	}

	format->fourcc = fourcc;
	format->width = cap.width;
	format->height = cap.height;
	format->interval = VideoFormat::FpsToInterval(cap.maxFPS);
	return true;
}

static bool FormatToCapability(const VideoFormat& format, webrtc::VideoCaptureCapability* cap) 
{
	webrtc::RawVideoType webrtc_type = webrtc::kVideoUnknown;
	for (size_t i = 0; i < ARRAY_SIZE(kSupportedFourCCs); ++i) 
	{
		if (kSupportedFourCCs[i].fourcc == format.fourcc) 
		{
			webrtc_type = kSupportedFourCCs[i].webrtc_type;
			break;
		}
	}
	if (webrtc_type == webrtc::kVideoUnknown) 
	{
		return false;
	}

	cap->width = format.width;
	cap->height = format.height;
	cap->maxFPS = VideoFormat::IntervalToFps(format.interval);
	cap->expectedCaptureDelay = 0;
	cap->rawType = webrtc_type;
	cap->codecType = webrtc::kVideoCodecUnknown;
	cap->interlaced = false;
	return true;
}

namespace vosvideo
{
	namespace camera
	{
		class IpCamVcmFactory : public CameraVcmFactoryInterface 
		{
		public:
			virtual webrtc::VideoCaptureModule* Create(int id, const char* device) 
			{
				return webrtc::VideoCaptureFactory::Create(id, device);
			}

			virtual webrtc::VideoCaptureModule* Create(const int32_t id, webrtc::VideoCaptureExternal*& externalCapture, CameraPlayer* player) 
			{
				return CameraVideoCaptureImpl::Create(id, externalCapture, player);
			}

			virtual webrtc::VideoCaptureModule::DeviceInfo* CreateDeviceInfo(int id) 
			{
				return webrtc::VideoCaptureFactory::CreateDeviceInfo(id);
			}

			virtual void DestroyDeviceInfo(webrtc::VideoCaptureModule::DeviceInfo* info) 
			{
				delete info;
			}
		};
	}
}



CameraVideoCapturer::CameraVideoCapturer() : 
	factory_(new IpCamVcmFactory),
	module_(NULL),
	captured_frames_(0) 
{
}

CameraVideoCapturer::CameraVideoCapturer(CameraVcmFactoryInterface* factory) : 
	factory_(factory),
	module_(NULL),
	captured_frames_(0) 
{
}

CameraVideoCapturer::~CameraVideoCapturer() 
{
	if (module_) 
	{
		module_->Release();
	}
}

bool CameraVideoCapturer::Init(int camId, CameraPlayer* device) 
{
	if (module_) 
	{
		LOG(LS_ERROR) << "The capturer is already initialized";
		return false;
	}
	VideoCaptureExternal *extCapturer = nullptr;
	module_ = factory_->Create(camId, extCapturer, device);
	if (!module_) 
	{
		LOG_ERROR("Failed to create capturer for id: " << device);
		return false;
	}

	// It is safe to change member attributes now.
	module_->AddRef();

	try
	{
		string strCamId = lexical_cast<string>(camId);
		SetId(strCamId);
	}
	catch(bad_lexical_cast&)
	{
		LOG_ERROR("Couldn't cast camera id from number to string");
		return false;
	}

	webrtc::VideoCaptureCapability cap;
	device->GetWebRtcCapability(cap);
	cap.rawType = webrtc::kVideoI420;
	vector<VideoFormat> supported;
	VideoFormat format;

	if (CapabilityToFormat(cap, &format)) 
	{
		supported.push_back(format);
	}

	SetSupportedFormats(supported);

	return true;
}

bool CameraVideoCapturer::Init(const Device& device) 
{
	if (module_) 
	{
		LOG(LS_ERROR) << "The capturer is already initialized";
		return false;
	}

	webrtc::VideoCaptureModule::DeviceInfo* info = factory_->CreateDeviceInfo(0);
	if (!info) 
	{
		return false;
	}

	// Find the desired camera, by name.
	// In the future, comparing IDs will be more robust.
	// TODO(juberti): Figure what's needed to allow this.
	int num_cams = info->NumberOfDevices();
	char vcm_id[256] = "";
	bool found = false;
	for (int index = 0; index < num_cams; ++index) 
	{
		char vcm_name[256];
		if (info->GetDeviceName(index, vcm_name, ARRAY_SIZE(vcm_name),
			vcm_id, ARRAY_SIZE(vcm_id)) != -1) 
		{
			if (device.name == reinterpret_cast<char*>(vcm_name)) 
			{
				found = true;
				break;
			}
		}
	}
	if (!found) 
	{
		LOG(LS_WARNING) << "Failed to find capturer for id: " << device.id;
		factory_->DestroyDeviceInfo(info);
		return false;
	}

	// Enumerate the supported formats.
	// TODO(juberti): Find out why this starts/stops the camera...
	vector<VideoFormat> supported;
	int32_t num_caps = info->NumberOfCapabilities(vcm_id);
	for (int32_t i = 0; i < num_caps; ++i) 
	{
		webrtc::VideoCaptureCapability cap;
		if (info->GetCapability(vcm_id, i, cap) != -1) 
		{
			VideoFormat format;
			if (CapabilityToFormat(cap, &format)) 
			{
				supported.push_back(format);
			}
			else 
			{
				LOG(LS_WARNING) << "Ignoring unsupported WebRTC capture format "<< cap.rawType;
			}
		}
	}
	factory_->DestroyDeviceInfo(info);
	if (supported.empty()) 
	{
		LOG(LS_ERROR) << "Failed to find usable formats for id: " << device.id;
		return false;
	}

	module_ = factory_->Create(0, vcm_id);
	if (!module_) 
	{
		LOG(LS_ERROR) << "Failed to create capturer for id: " << device.id;
		return false;
	}

	// It is safe to change member attributes now.
	module_->AddRef();
	SetId(device.id);
	SetSupportedFormats(supported);
	return true;
}

bool CameraVideoCapturer::Init(webrtc::VideoCaptureModule* module) 
{
	if (module_) 
	{
		LOG(LS_ERROR) << "The capturer is already initialized";
		return false;
	}
	if (!module) 
	{
		LOG(LS_ERROR) << "Invalid VCM supplied";
		return false;
	}
	// TODO(juberti): Set id and formats.
	(module_ = module)->AddRef();
	return true;
}

bool CameraVideoCapturer::GetBestCaptureFormat(const VideoFormat& desired,
											  VideoFormat* best_format) 
{
	if (!best_format) 
	{
		return false;
	}

	if (!VideoCapturer::GetBestCaptureFormat(desired, best_format)) 
	{
		// If the vcm has a list of the supported format, but didn't find the
		// best match, then we should return fail.
		if (GetSupportedFormats()) 
		{
			return false;
		}

		// We maybe using a manually injected VCM which doesn't support enum.
		// Use the desired format as the best format.
		best_format->width = desired.width;
		best_format->height = desired.height;
		best_format->fourcc = FOURCC_I420;
		best_format->interval = desired.interval;
		LOG(LS_INFO) << "Failed to find best capture format," << " fall back to the requested format "<< best_format->ToString();
	}
	return true;
}

CaptureState CameraVideoCapturer::Start(const VideoFormat& capture_format) 
{
	if (!module_) 
	{
		LOG(LS_ERROR) << "The capturer has not been initialized";
		return CS_NO_DEVICE;
	}

	// TODO(hellner): weird to return failure when it is in fact actually running.
	if (IsRunning()) 
	{
		LOG(LS_ERROR) << "The capturer is already running";
		return CS_FAILED;
	}

	SetCaptureFormat(&capture_format);

	webrtc::VideoCaptureCapability cap;
	if (!FormatToCapability(capture_format, &cap)) 
	{
		LOG(LS_ERROR) << "Invalid capture format specified";
		return CS_FAILED;
	}

	string camera_id(GetId());
	uint32 start = talk_base::Time();
	module_->RegisterCaptureDataCallback(*this);

	if (module_->StartCapture(cap) != 0) 
	{
		LOG(LS_ERROR) << "Camera '" << camera_id << "' failed to start";
		return CS_FAILED;
	}

	LOG(LS_INFO) << "Camera '" << camera_id << "' started with format "<< capture_format.ToString() << ", elapsed time "<< talk_base::TimeSince(start) << " ms";

	captured_frames_ = 0;
	SetCaptureState(CS_RUNNING);
	return CS_STARTING;
}

void CameraVideoCapturer::Stop() 
{
	if (IsRunning()) 
	{
		talk_base::Thread::Current()->Clear(this);
		module_->StopCapture();
		module_->DeRegisterCaptureDataCallback();

		// TODO(juberti): Determine if the VCM exposes any drop stats we can use.
		double drop_ratio = 0.0;
		string camera_id(GetId());
		LOG(LS_INFO) << "Camera '" << camera_id << "' stopped after capturing "<< captured_frames_ << " frames and dropping "<< drop_ratio << "%";
	}
	SetCaptureFormat(NULL);
}

bool CameraVideoCapturer::IsRunning() 
{
	return (module_ != nullptr && module_->CaptureStarted());
}

bool CameraVideoCapturer::GetPreferredFourccs(vector<uint32>* fourccs) 
{
	if (!fourccs) 
	{
		return false;
	}

	fourccs->clear();
	for (size_t i = 0; i < ARRAY_SIZE(kSupportedFourCCs); ++i) 
	{
		fourccs->push_back(kSupportedFourCCs[i].fourcc);
	}
	return true;
}

void CameraVideoCapturer::OnCaptureDelayChanged(const int32_t id, const int32_t delay) 
{
	LOG(LS_INFO) << "Capture delay changed to " << delay << " ms";
}

void CameraVideoCapturer::OnIncomingCapturedFrame(const int32_t id, I420VideoFrame& sample)
{
	++captured_frames_;
	// Signal down stream components on captured frame.
	// The CapturedFrame class doesn't support planes. We have to ExtractBuffer
	// to one block for it.
	int length = webrtc::CalcBufferSize(webrtc::kI420, sample.width(), sample.height());
	capture_buffer_.resize(length);
	webrtc::ExtractBuffer(sample, length, &capture_buffer_[0]);
	WebRtcCapturedFrame frame(sample, &capture_buffer_[0], length);
	// This delay fix bug in WebRTC layer when frame time stamp 
	// already assigned to "last" timestamp but after this call it 
	// recursively returns back and check if time stamp != "last" 
	// On fast computers call gets back in less then one millisecond 
	// which means frame will be dropped. This delay fix the problem.
	Sleep(1); 
	
	SignalFrameCaptured(this, &frame);
}


