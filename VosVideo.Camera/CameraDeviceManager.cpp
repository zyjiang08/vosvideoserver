#include "stdafx.h"
#include <talk/media/devices/win32devicemanager.h>

#include <atlbase.h>
#include <dbt.h>
#include <strmif.h>  // must come before ks.h
#include <ks.h>
#include <ksmedia.h>
#define INITGUID  // For PKEY_AudioEndpoint_GUID
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <functiondiscoverykeys_devpkey.h>
#include <uuids.h>
#include <mferror.h>

#include <boost/format.hpp>
#include <boost/assign/list_inserter.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <talk/base/logging.h>
#include <talk/base/stringutils.h>
#include <talk/base/thread.h>
#include <talk/base/win32.h>  // ToUtf8
#include <talk/base/win32window.h>
#include <talk/media/base/mediacommon.h>
#include <talk/media/webrtc/webrtcvideocapturer.h>
#include <Poco/Process.h>
#include <vosvideocommon/StringUtil.h>
#include <vosvideocommon/NativeErrorsManager.h>

#include "VosVideo.Communication/TypeInfoWrapper.h"
#include "VosVideo.Data/WebRtcIceCandidateMsg.h"
#include "VosVideo.Data/DeletePeerConnectionRequestMsg.h"
#include "VosVideo.Data/WebsocketConnectionClosedMsg.h"
#include "VosVideo.Data/LiveVideoOfferMsg.h"
#include "VosVideo.Data/SdpAnswerMsg.h"
#include "VosVideo.Data/IceCandidateResponseMsg.h"
#include "CameraDeviceManager.h"
#include "CameraVideoCapturer.h"

using namespace std;
using namespace util;
using boost::wformat;
using boost::lexical_cast;
using boost::bad_lexical_cast;
using namespace concurrency;
using namespace boost::assign;
using namespace vosvideo::camera;
using namespace vosvideo::devicemanagement;
using namespace vosvideo::data;
using namespace vosvideo::communication;
using namespace cricket;


CameraDeviceManager::CameraDeviceManager()
{
	Init();
}

CameraDeviceManager::CameraDeviceManager(std::shared_ptr<CommunicationManager> commManager,
										std::shared_ptr<vosvideo::devicemanagement::DeviceConfigurationManager> devConfMgr,
										std::shared_ptr<vosvideo::communication::PubSubService> pubSubService, 
										std::shared_ptr<vosvideo::usermanagement::UserManager> userMgr, 
									    std::shared_ptr<vosvideo::configuration::ConfigurationManager> configMgr) : 
	commManager_(commManager),
	pubSubService_(pubSubService),
	devConfMgr_(devConfMgr),
	userMgr_(userMgr),
	configMgr_(configMgr)
{
	vector<TypeInfoWrapper> interestedTypes;

	TypeInfoWrapper typeInfo = typeid(WebRtcIceCandidateMsg);	
	interestedTypes.push_back(typeInfo);

	typeInfo = typeid(LiveVideoOfferMsg);
	interestedTypes.push_back(typeInfo);

	typeInfo = typeid(DeletePeerConnectionRequestMsg);
	interestedTypes.push_back(typeInfo);

	typeInfo = typeid(WebsocketConnectionClosedMsg);
	interestedTypes.push_back(typeInfo);

	typeInfo = typeid(SdpAnswerMsg);
	interestedTypes.push_back(typeInfo);
	
	typeInfo = typeid(IceCandidateResponseMsg);
	interestedTypes.push_back(typeInfo);

	pubSubService_->Subscribe(interestedTypes, *this);

	Init();
	devConfMgr_->ConnectToDeviceUpdateSignal(boost::bind(&CameraDeviceManager::OnCameraUpdate, this, _1));
}

void CameraDeviceManager::OnMessageReceived(const shared_ptr<ReceivedData> receivedMessage)
{	
	wstring srvPeer;
	wstring clientPeer;
	receivedMessage->GetFromPeer(clientPeer);
	receivedMessage->GetToPeer(srvPeer);

	wstring msgBody = receivedMessage->ToString();
	int devId;
	web::json::value mediaObj;

	if(dynamic_pointer_cast<LiveVideoOfferMsg>(receivedMessage) ||
	   dynamic_pointer_cast<WebRtcIceCandidateMsg>(receivedMessage))
	{
		shared_ptr<MediaInfo> msgPtr = dynamic_pointer_cast<MediaInfo>(receivedMessage);
		msgPtr->GetMediaInfo(mediaObj);
		GetDeviceIdFromJson(devId, mediaObj);
		CameraPlayerProcessMap::iterator iter = cameraProcess_.find(devId);
		if (iter != cameraProcess_.end())
		{
			iter->second->Send(msgBody);
		}
	}
	else if (dynamic_pointer_cast<WebRtcIceCandidateMsg>(receivedMessage))
	{
		shared_ptr<WebRtcIceCandidateMsg> msgPtr = dynamic_pointer_cast<WebRtcIceCandidateMsg>(receivedMessage);
		msgPtr->GetMediaInfo(mediaObj);
		GetDeviceIdFromJson(devId, mediaObj);
		CameraPlayerProcessMap::iterator iter = cameraProcess_.find(devId);
		if (iter != cameraProcess_.end())
		{
			iter->second->Send(msgBody);
		}
	}
	else if(dynamic_pointer_cast<WebsocketConnectionClosedMsg>(receivedMessage) ||
		    dynamic_pointer_cast<DeletePeerConnectionRequestMsg>(receivedMessage))
	{
		for(CameraPlayerProcessMap::iterator iter = cameraProcess_.begin(); iter != cameraProcess_.end(); ++iter)
		{
			iter->second->Send(msgBody);
		}
	}
	else if(dynamic_pointer_cast<SdpAnswerMsg>(receivedMessage) ||
		    dynamic_pointer_cast<IceCandidateResponseMsg>(receivedMessage))
	{
		string smsg;
		StringUtil::ToString(receivedMessage->ToString(), smsg);
		commManager_->WebsocketSend(smsg);
	}
}

void CameraDeviceManager::PassMessage(web::json::value& mediaObj, const wstring& payload)
{
	int devId;
	GetDeviceIdFromJson(devId, mediaObj);
	CameraPlayerProcessMap::iterator procIter = cameraProcess_.find(devId);
	if (procIter != cameraProcess_.end())
	{
		procIter->second->Send(payload);
	}
}

void CameraDeviceManager::Shutdown()
{
	lock_guard<std::mutex> lock(mutex_);
	reconnectTimer_->stop();

	for_each (cameraProcess_.begin(), cameraProcess_.end(), [](pair<int, std::shared_ptr<CameraPlayerProcess> > p)
	{
		p.second->Shutdown();
	}
	);

	//for_each (cameraPlayers_.begin(), cameraPlayers_.end(), [](pair<int, CameraPlayer*>p)
	//{
	//	p.second->Shutdown();
	//	// Destroy via COM facility
	//	p.second->Release();
	//}
	//);

	if (initialized())
	{
		Terminate();
	}
}

CameraDeviceManager::~CameraDeviceManager()
{
}

void CameraDeviceManager::AddIpCam(web::json::value& camParms)
{
	int camId;
	CameraConfMsg conf;
	CreateCameraConfFromJson(camId, conf, camParms);
	CreatePlayerProcess(conf);
}

void CameraDeviceManager::RemoveIpCam(int camId)
{
	CameraPlayerProcessMap::iterator iter = cameraProcess_.find(camId);

	if (iter != cameraProcess_.end())
	{
		iter->second->Shutdown();
	}
}

void CameraDeviceManager::GetDeviceIdFromJson(int& camId, web::json::value& camParms)
{
	camId = -1;
	web::json::value::iterator it;

	for(it = camParms.begin(); it != camParms.end(); ++it)
	{
		wstring key = (*it).first.as_string();
		web::json::value value = (*it).second;

		if (key == L"DeviceId" && value.is_number())
		{
			camId = value.as_integer();
			break;
		}
	}
}

void CameraDeviceManager::CreateCameraConfFromJson(int& camId, CameraConfMsg& conf, web::json::value& camParms)
{
	vector<wstring> strVideoUri(3);
	wstring customUri;
	bool    isActive = false;
	int     camPort;
	CameraType modelType = CameraType::UNKNOWN;
	int     recordLen = 1; // default 1 min
	wstring devName;
	wstring audioUri;
	wstring camIpAddr;
	wstring camUsername;
	wstring camPass;
	CameraVideoFormat uriType = CameraVideoFormat::UNKNOWN;
	CameraVideoRecording recordingType = CameraVideoRecording::DISABLED;
	web::json::value::iterator it;
	
	for(it = camParms.begin(); it != camParms.end(); ++it)
	{
		wstring key = (*it).first.as_string();
		web::json::value value = (*it).second;
		// For video url we should make decision
		if (key == L"DeviceName")
		{
			if(value.is_string())
				devName = value.as_string();
		}
		if (key == L"DeviceId")
		{
			if(value.is_number())
				camId = value.as_integer();
		}
		else if (key == L"CustomUri")
		{
			if(value.is_string())
				customUri = value.as_string();
		}
		else if (key == L"MjpegUri")
		{
			if(value.is_string())
				strVideoUri[static_cast<int>(CameraVideoFormat::MJPEG)] = value.as_string();
		}
		else if (key == L"Mpeg4Uri")
		{
			if(value.is_string())
				strVideoUri[static_cast<int>(CameraVideoFormat::MPEG4)] = value.as_string();
		}
		else if (key == L"H264Uri")
		{
			if(value.is_string())
				strVideoUri[static_cast<int>(CameraVideoFormat::H264)] = value.as_string();
		}
		else if (key == L"RecordingType")
		{
			if(value.is_number())
			{
				int rType = value.as_integer();
				recordingType = static_cast<CameraVideoRecording>(rType);
			}
		}
		else if (key == L"RecordingLength")
		{
			if(value.is_number())
			{
				recordLen = value.as_integer();
			}
		}
		else if (key == L"UriType")
		{
			if(value.is_number())
			{
				int uType = value.as_integer();
				uriType = static_cast<CameraVideoFormat>(uType);
			}
		}
		else if (key == L"AudioUri")
		{
			if(value.is_string())
				audioUri = value.as_string();
		}
		else if (key == L"IpAddress")
		{
			if(value.is_string())
				camIpAddr = value.as_string();
		}
		else if (key == L"Port")
		{
			if(value.is_number())
				camPort = value.as_integer();
		}
		else if (key == L"DeviceUserName")
		{
			if(value.is_string())
				camUsername = value.as_string();
		}
		else if (key == L"DevicePassword")
		{
			if(value.is_string())
				camPass = value.as_string();
		}
		else if (key == L"IsActive")
		{
			if(value.is_boolean())
				isActive = value.as_bool();
		}
		else if (key == L"ModelType")
		{
			if(value.is_number())
				modelType = static_cast<CameraType>(value.as_integer());
		}
	}

	// Custom URI has higher priority, respect it 
	wstring videoUri = customUri.length() > 0 ? customUri : strVideoUri[static_cast<int>(uriType)];

	if (modelType == CameraType::IPCAM)
	{
		// it is possible that URI has no '/' symbol at the beginning. Test for it and fix if problem is here 
		if (videoUri.at(0) != L'/')
		{
			videoUri = L"/" + videoUri;
		}

		// VosVideo uses own uri schema for rtsp protocol which is MPEG4 and H264 cameras
		// should be RTSPX instead RTSP, make needed correction
		videoUri = str(wformat(L"%1%://%2%:%3%%4%") % (uriType == CameraVideoFormat::MJPEG ? L"httpx" : L"rtspx") % camIpAddr % camPort % videoUri);
	}

	conf = CameraConfMsg(modelType, uriType);
	conf.SetIsActive(isActive);
	conf.SetCameraIds(camId, devName);
	conf.SetCredentials(camUsername, camPass);
	conf.SetUris(audioUri, videoUri);
	conf.SetFileSinkParameters(configMgr_->GetArchivePath(), recordLen, recordingType);
}

void CameraDeviceManager::OnCameraUpdate(web::json::value& camArr)
{
	web::json::value::iterator camIter;

	// Quite possible situation that user click twice on save button
	// Just to make sure protect this block
	lock_guard<std::mutex> lock(mutex_);

	// STEP 1: Pessimistic scenario
	for(CameraConfsMap::iterator confIter = cameraConfs_.begin(); confIter != cameraConfs_.end(); ++confIter)
	{
		confIter->second.second = DeviceConfigurationFlag::REMOVED;
	}

	// STEP 2: Mark with actual status
	for(camIter = camArr.begin(); camIter != camArr.end(); ++camIter)
	{
		int camId;
		CameraConfMsg ipConf;
		CreateCameraConfFromJson(camId, ipConf, camIter->second);
		//
		// Here we implementing very simple algo
		// All new cam conf has flag NEW
		// Not changed NOCHANGE
		// Changed CHANGED accordingly
		// On next step all this flags get turned to PROCESSED
		// Next time if processed flag found it means camera was removed
		//
		CameraConfsMap::iterator iter = cameraConfs_.find(camId);

		// Camera exists and no changes found, nothing to do
		if (iter != cameraConfs_.end() && (*iter).second.first == ipConf) // NOCHANGE
		{
			DeviceConfigurationFlag& flag = iter->second.second;
			flag = DeviceConfigurationFlag::NOCHANGE;
		}
		else if (iter == cameraConfs_.end())// ADDED
		{
			cameraConfs_.insert(make_pair(camId, make_pair(ipConf, DeviceConfigurationFlag::ADDED)));
		}
		else if (iter != cameraConfs_.end() && iter->second.first != ipConf) // UPDATED
		{
			CameraConfMsg &conf = iter->second.first;
			conf = ipConf;
			DeviceConfigurationFlag& flag = iter->second.second;
			flag = DeviceConfigurationFlag::UPDATED;
		}
	}


	// STEP 3: Activate accordingly STEP 2
	CameraConfsMap::iterator confIter = cameraConfs_.begin();
	while (confIter != cameraConfs_.end()) 
	{
		if ((*confIter).second.second == DeviceConfigurationFlag::ADDED)
		{
			try
			{
				CreatePlayerProcess(confIter->second.first);
				confIter->second.second = DeviceConfigurationFlag::NOCHANGE;
			}
			catch (CameraException& e)
			{			
				LOG_ERROR(e.what());
				NotifyAllUsers(confIter->second.first, e);
			}
		}		
		else  if (confIter->second.second == DeviceConfigurationFlag::UPDATED) // Changed camera remove and recreate
		{
			DeletePlayerProcess(confIter->first);

			try
			{
				CreatePlayerProcess(confIter->second.first);
				confIter->second.second = DeviceConfigurationFlag::NOCHANGE;
			}
			catch (CameraException& e)
			{			
				LOG_ERROR(e.what());
				NotifyAllUsers(confIter->second.first, e);
			}
		}
		confIter++;
	}

	// STEMP 4: Remove unknown cameras
	// Whatever is left marked as REMOVED should go away
	CameraConfsMap::iterator removeIter = cameraConfs_.begin();
	while (removeIter != cameraConfs_.end()) 
	{
		if (removeIter->second.second == DeviceConfigurationFlag::REMOVED)// Removed camera, need to stop it and remove
		{
			DeletePlayerProcess(removeIter->first);
			removeIter = cameraConfs_.erase(removeIter);
		}
		else
		{
			++removeIter;
		}
	}
}

// Shortcut
void CameraDeviceManager::NotifyAllUsers(const CameraConfMsg& conf, const CameraException& e)
{
	int cameraId;
	wstring cameraName;
	conf.GetCameraIds(cameraId, cameraName);
	string errMsg(e.what());
	wstring werrMsg;
	StringUtil::ToWstring(errMsg, werrMsg);
	shared_ptr<RtbcDeviceErrorOutMsg> camErr(new RtbcDeviceErrorOutMsg(cameraId, werrMsg));
	userMgr_->NotifyAllUsers(camErr);
}

void CameraDeviceManager::CreatePlayerProcess(CameraConfMsg& conf)
{
	int cameraId; 
	wstring cameraName;
	conf.GetCameraIds(cameraId, cameraName);

	if (conf.GetIsActive() == false)
	{
		LOG_TRACE("Camera " << cameraName << " is not an active. Skipped creation.");
		return;
	}

	std::shared_ptr<CameraPlayerProcess> cp(new CameraPlayerProcess(pubSubService_, conf));
	cameraProcess_.insert(make_pair( cameraId, cp));
}

bool CameraDeviceManager::Init()
{
	auto callback = new call<CameraDeviceManager*>([this](CameraDeviceManager*)
	{
		this->ReconnectCamera();
	});

	reconnectTimer_ = new Concurrency::timer<CameraDeviceManager*>(reconnectTimeout_, 0, callback, true);
	reconnectTimer_->start();
	return true;
}

void CameraDeviceManager::ReconnectCamera()
{
	lock_guard<std::mutex> lock(mutex_);

	for (CameraPlayerProcessMap::iterator iter = cameraProcess_.begin(); iter != cameraProcess_.end(); ++iter)
	{
		// Internally check if process is dead
		iter->second->Reconnect();
	}
}


//void CameraDeviceManager::ReconnectCamera()
//{
//	lock_guard<std::mutex> lock(mutex_);
//
//	vector< pair<int, CameraPlayer*> > candidates;
//
//	for (CameraPlayersMap::iterator iter = cameraPlayers_.begin(); iter != cameraPlayers_.end(); ++iter)
//	{
//		if (iter->second->GetState() == PlayerState::Stopped || 
//			iter->second->GetState() == PlayerState::Closed)
//		{
//			candidates.push_back(make_pair(iter->first, iter->second));
//		}
//	}
//
//	for (vector<pair<int, CameraPlayer*>>::iterator iter = candidates.begin(); iter != candidates.end(); ++iter)
//	{
//		DeletePlayerProcess(iter->first);
//		CameraConfsMap::iterator conf = cameraConfs_.find(iter->first);
//
//		if (conf != cameraConfs_.end())
//		{
//			CreatePlayerProcess(conf->second.first);
//		}
//	}
//}

void CameraDeviceManager::DeletePlayerProcess(int devId)
{
	CameraPlayerProcessMap::iterator processIter = cameraProcess_.find(devId);
	if (processIter != cameraProcess_.end())
	{
		processIter->second->Shutdown();
		cameraProcess_.erase(processIter->first);
	}

}

void CameraDeviceManager::Terminate()
{
}

bool CameraDeviceManager::GetDefaultVideoCaptureDevice(Device* device)
{
	bool ret = false;
	// If there are multiple capture devices, we want the first USB one.
	// This avoids issues with defaulting to virtual cameras or grabber cards.
	std::vector<Device> devices;
	if (!cameraPlayers_.empty())
	{
		CameraPlayersMap::iterator iter = cameraPlayers_.begin();
		string camId;
		try
		{
			camId = lexical_cast<string>(iter->first); 
		}
		catch(bad_lexical_cast&)
		{
			LOG_ERROR("Couldn't cast camera id from number to string");
			return false;
		}
		device = new Device(camId, camId);
		ret = true;
	}
	return ret;
}

bool CameraDeviceManager::GetAudioDevices(bool input, std::vector<Device>* devs)
{
	devs->clear();

	return false;
}

bool CameraDeviceManager::GetVideoCaptureDevices(std::vector<Device>* devices)
{
	devices->clear();
	if (!cameraPlayers_.empty())
	{
		for_each (cameraPlayers_.begin(), cameraPlayers_.end(), [devices](pair<int, CameraPlayer*>p)
		{
			string camId;
			try
			{
				camId = lexical_cast<string>(p.first); 
			}
			catch(bad_lexical_cast&)
			{
				LOG_ERROR("Couldn't cast camera id from number to string");
				return;
			}
			devices->push_back(Device(camId, camId));
		}
		);

		return true;
	}

	return false;
}

bool CameraDeviceManager::IsVideoCaptureDeviceExists(int devId)
{
	lock_guard<std::mutex> lock(mutex_);

	CameraPlayersMap::iterator iter = cameraPlayers_.find(devId);

	if (iter != cameraPlayers_.end())
	{
		return true;
	}

	return false;
}

bool CameraDeviceManager::IsVideoCaptureDeviceReady(int devId, shared_ptr<SendData>& lastErrMsg)
{
	lock_guard<std::mutex> lock(mutex_);

	CameraPlayersMap::iterator iter = cameraPlayers_.find(devId);

	if (iter != cameraPlayers_.end())
	{
		PlayerState pstate = iter->second->GetState(lastErrMsg);
		return !(pstate == PlayerState::Closed || pstate == PlayerState::Closing || pstate == PlayerState::OpenPending);
	}

	return false;
}

bool CameraDeviceManager::GetVideoCaptureDevice(int devId, Device& device)
{
	lock_guard<std::mutex> lock(mutex_);

	CameraPlayersMap::iterator iter = cameraPlayers_.find(devId);

	if (iter != cameraPlayers_.end())
	{
		string camId;
		try
		{
			camId = lexical_cast<string>(iter->first); 
		}
		catch(bad_lexical_cast&)
		{
			LOG_ERROR("Couldn't cast camera id from number to string");
			return false;
		}

		device = Device(camId, camId);
		return true;
	}

	return false;
}

VideoCapturer* CameraDeviceManager::CreateVideoCapturer(const Device& device) const
{
	int camId = 0;
	try
	{
		camId = lexical_cast<int>(device.id);	 
	}
	catch (bad_lexical_cast&)
	{
		LOG_ERROR("Couldn't cast camera id from string to number: " << device.id);
		return NULL;
	}
	
	CameraPlayersMap::const_iterator iter =  cameraPlayers_.find(camId);
	// Probably impossible but lets make sure
	if (iter == cameraPlayers_.end())
	{
		return NULL;
	}

	CameraVideoCapturer* capturer = new CameraVideoCapturer();
	if (!capturer->Init(iter->first, iter->second)) 
	{
		delete capturer;
		return NULL;
	}
	return capturer;
}

