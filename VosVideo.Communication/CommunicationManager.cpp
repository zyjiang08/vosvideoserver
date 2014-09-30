#include "stdafx.h"
#include <boost/format.hpp>
#include <vosvideocommon/StringUtil.h>
#include "CommunicationManager.h"

using namespace std;
using namespace util;
using namespace vosvideo::data;
using boost::format;
using vosvideo::communication::CommunicationManager;
using vosvideo::communication::HttpClient;
using vosvideo::communication::WebsocketClient;

const string CommunicationManager::msgFormat_ = "{\"fp\":\"%1%\",\"tp\":\"%2%\",\"mt\":%3%,\"m\":%4%}";

CommunicationManager::CommunicationManager(std::shared_ptr<HttpClient> httpClient, std::shared_ptr<WebsocketClient> websocketClient) 
	: httpClient_(httpClient), 
	  websocketClient_(websocketClient)
{
}


CommunicationManager::~CommunicationManager(void)
{
}

concurrency::task<web::json::value> CommunicationManager::HttpGet(std::wstring const& path)
{
	return httpClient_->Get(path);
}

concurrency::task<web::json::value> CommunicationManager::HttpPost(std::wstring const& path, web::json::value const& payload)
{
	return httpClient_->Post(path, payload);
}

void vosvideo::communication::CommunicationManager::WebsocketConnect(std::wstring const& path)
{
	websocketClient_->Connect(path);
}

void CommunicationManager::WebsocketSend(std::string const& message)
{
	websocketClient_->Send(message);
}

void CommunicationManager::WebsocketClose()
{
	websocketClient_->Close();
}

string CommunicationManager::StringToJson(string str)
{
	return "\"" + str + "\"";
}

void CommunicationManager::CreateWebsocketMessageString(const std::wstring& fromPeer, 
														const std::wstring& toPeer, 
														shared_ptr<SendData> outMsg, 
														std::string& returnedMessage)
{
	string sfromPeer, stoPeer, body;
	StringUtil::ToString(fromPeer, sfromPeer);
	StringUtil::ToString(toPeer, stoPeer);
	wstring wbody;
	outMsg->GetAsJsonString(wbody);
	StringUtil::ToString(wbody, body);
	MsgType msgType = outMsg->GetMsgType();

	returnedMessage = boost::str(format(msgFormat_) % sfromPeer % stoPeer % static_cast<int>(msgType) % body);
}

void CommunicationManager::CreateWebsocketMessageString(const std::wstring& fromPeer, 
													  const std::wstring& toPeer, 
													  vosvideo::data::MsgType msgType, 
													  const std::string& body, 
													  std::string& returnedMessage)
{
	string sfromPeer, stoPeer;
	StringUtil::ToString(fromPeer, sfromPeer);
	StringUtil::ToString(toPeer, stoPeer);

	returnedMessage = boost::str(format(msgFormat_) % sfromPeer % stoPeer % static_cast<int>(msgType) % body);
}
