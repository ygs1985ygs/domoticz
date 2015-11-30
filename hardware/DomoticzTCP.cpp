#include "stdafx.h"
#include "DomoticzTCP.h"
#include "../main/Logger.h"
#include "../main/Helper.h"
#include "../main/localtime_r.h"
#include "../main/mainworker.h"
#include "../main/WebServerHelper.h"

#define RETRY_DELAY 30

extern http::server::CWebServerHelper m_webservers;

DomoticzTCP::DomoticzTCP(const int ID, const std::string &IPAddress, const unsigned short usIPPort, const std::string &username, const std::string &password):
m_username(username), m_password(password), m_szIPAddress(IPAddress)
{
	m_HwdID=ID;
	m_socket=INVALID_SOCKET;
	m_stoprequested=false;
	m_usIPPort=usIPPort;
	info = NULL;
	b_useProxy = IsValidAPIKey(IPAddress);
	b_ProxyConnected = false;
}

DomoticzTCP::~DomoticzTCP(void)
{
#if defined WIN32
	//
	// Release WinSock
	//
#endif
	if (NULL != info) {
		freeaddrinfo(info);
	}

}

bool DomoticzTCP::IsValidAPIKey(const std::string &IPAddress)
{
	if (IPAddress.find(".") != std::string::npos) {
		// we assume an IPv4 address or host name
		return false;
	}
	if (IPAddress.find(":") != std::string::npos) {
		// we assume an IPv6 address
		return false;
	}
	// just a simple check
	return IPAddress.length() == 15;
}

#if defined WIN32
int inet_pton(int af, const char *src, void *dst)
{
  struct sockaddr_storage ss;
  int size = sizeof(ss);
  char src_copy[INET6_ADDRSTRLEN+1];

  ZeroMemory(&ss, sizeof(ss));
  /* stupid non-const API */
  strncpy (src_copy, src, INET6_ADDRSTRLEN+1);
  src_copy[INET6_ADDRSTRLEN] = 0;

  if (WSAStringToAddress(src_copy, af, NULL, (struct sockaddr *)&ss, &size) == 0) {
    switch(af) {
      case AF_INET:
    *(struct in_addr *)dst = ((struct sockaddr_in *)&ss)->sin_addr;
    return 1;
      case AF_INET6:
    *(struct in6_addr *)dst = ((struct sockaddr_in6 *)&ss)->sin6_addr;
    return 1;
    }
  }
  return 0;
}
#endif

bool DomoticzTCP::StartHardware()
{
	if (b_useProxy) {
		return StartHardwareProxy();
	}
	else {
		return StartHardwareTCP();
	}
}

bool DomoticzTCP::StartHardwareTCP()
{
	int rc;
	struct addrinfo hints;
	m_bIsStarted=true;

	m_stoprequested=false;

	memset(&m_addr,0,sizeof(sockaddr_in6));
	m_addr.sin6_family = AF_INET6;
	m_addr.sin6_port = htons(m_usIPPort);

	// RK, removed: unsigned long ip;
	memset(&hints, 0x00, sizeof(hints));
    hints.ai_flags    = AI_NUMERICSERV;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    rc = inet_pton(AF_INET, m_szIPAddress.c_str(), &(m_addr.sin6_addr));
	if (rc == 1)    /* valid IPv4 text address? */
	{
		hints.ai_family = AF_INET;
		hints.ai_flags |= AI_NUMERICHOST;
	}
	else
	{
		rc = inet_pton(AF_INET6, m_szIPAddress.c_str(), &(m_addr.sin6_addr));
		if (rc == 1) /* valid IPv6 text address? */
		{

		hints.ai_family = AF_INET6;
		hints.ai_flags |= AI_NUMERICHOST;
		}
	}
	char myPort[256];
	sprintf(myPort, "%d", m_usIPPort);
	rc = getaddrinfo(m_szIPAddress.c_str(), myPort, &hints, &info);
	if (rc != 0)
	{
		return false;
	}

	m_retrycntr=RETRY_DELAY; //will force reconnect first thing

	//Start worker thread
	m_thread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&DomoticzTCP::Do_Work, this)));

	return (m_thread!=NULL);
}

bool DomoticzTCP::StopHardware()
{
	if (b_useProxy) {
		return StopHardwareProxy();
	}
	else {
		return StopHardwareTCP();
	}
}

bool DomoticzTCP::StopHardwareTCP()
{
	if (isConnected())
	{
		try {
			disconnectTCP();
		} catch(...)
		{
			//Don't throw from a Stop command
		}
	}
	else {
		try {
			if (m_thread)
			{
				m_stoprequested = true;
				m_thread->join();
			}
		}
		catch (...)
		{
			//Don't throw from a Stop command
		}
	}
	m_bIsStarted=false;
	return true;
}

bool DomoticzTCP::ConnectInternal()
{
	m_socket = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
	if (m_socket == INVALID_SOCKET)
	{
		_log.Log(LOG_ERROR,"Domoticz: TCP could not create a TCP/IP socket!");
		return false;
	}
/*
	//Set socket timeout to 2 minutes
#if !defined WIN32
	struct timeval tv;
	tv.tv_sec = 120;
	setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO,(struct timeval *)&tv,sizeof(struct timeval));
#else
	unsigned long nTimeout = 120*1000;
	setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&nTimeout, sizeof(DWORD));
#endif
*/
	// connect to the server
	int nRet;
	nRet = connect(m_socket,info->ai_addr, info->ai_addrlen);
	if (nRet == SOCKET_ERROR)
	{
		closesocket(m_socket);
		m_socket=INVALID_SOCKET;
		_log.Log(LOG_ERROR,"Domoticz: TCP could not connect to: %s:%ld",m_szIPAddress.c_str(),m_usIPPort);
		return false;
	}

	_log.Log(LOG_STATUS, "Domoticz: TCP connected to: %s:%ld",m_szIPAddress.c_str(),m_usIPPort);

	if (m_username!="")
	{
		char szAuth[300];
		sprintf(szAuth,"AUTH;%s;%s",m_username.c_str(),m_password.c_str());
		WriteToHardware((const char*)&szAuth,(const unsigned char)strlen(szAuth));
	}

	sOnConnected(this);
	return true;
}

void DomoticzTCP::disconnectTCP()
{
	m_stoprequested=true;
	if (m_socket!=INVALID_SOCKET)
	{
		closesocket(m_socket);	//will terminate the thread
		m_socket=INVALID_SOCKET;
		sleep_seconds(1);
	}
	//m_thread-> join();
}

void DomoticzTCP::Do_Work()
{
	char buf[100];
	int sec_counter = 0;
	while (!m_stoprequested)
	{
		if (
			(m_socket == INVALID_SOCKET)&&
			(!m_stoprequested)
			)
		{
			sleep_seconds(1);
			sec_counter++;

			if (sec_counter % 12 == 0) {
				mytime(&m_LastHeartbeat);
			}

			if (m_stoprequested)
				break;
			m_retrycntr++;
			if (m_retrycntr>=RETRY_DELAY)
			{
				m_retrycntr=0;
				if (!ConnectInternal())
				{
					_log.Log(LOG_STATUS,"Domoticz: retrying in %d seconds...",RETRY_DELAY);
				}
			}
		}
		else
		{
			//this could take a long time... maybe there will be no data received at all,
			//so it's no good to-do the heartbeat timing here
			m_LastHeartbeat = mytime(NULL);

			int bread=recv(m_socket,(char*)&buf,sizeof(buf),0);
			if (m_stoprequested)
				break;
			if (bread<=0) {
				_log.Log(LOG_ERROR,"Domoticz: TCP/IP connection closed! %s",m_szIPAddress.c_str());
				closesocket(m_socket);
				m_socket=INVALID_SOCKET;
				if (!m_stoprequested)
				{
					_log.Log(LOG_STATUS,"Domoticz: retrying in %d seconds...",RETRY_DELAY);
					m_retrycntr=0;
					continue;
				}
			}
			else
			{
				boost::lock_guard<boost::mutex> l(readQueueMutex);
				onRFXMessage((const unsigned char *)&buf,bread);
			}
		}
		
	}
	_log.Log(LOG_STATUS,"Domoticz: TCP/IP Worker stopped...");
} 

void DomoticzTCP::writeTCP(const char *data, size_t size)
{
	if (m_socket==INVALID_SOCKET)
		return; //not connected!
	send(m_socket,data,size,0);
}

bool DomoticzTCP::WriteToHardware(const char *pdata, const unsigned char length)
{
	if (b_useProxy) {
		if (isConnectedProxy()) {
			writeProxy(pdata, length);
			return true;
		}
	}
	else {
		if (isConnectedTCP())
		{
			writeTCP(pdata, length);
			return true;
		}
	}
	return false;
}

bool DomoticzTCP::isConnectedTCP()
{
	return m_socket != INVALID_SOCKET;
}

bool DomoticzTCP::isConnected()
{
	if (b_useProxy) {
		return isConnectedProxy();
	}
	else {
		return isConnectedTCP();
	}
}

bool DomoticzTCP::CompareToken(const std::string &aToken)
{
	return (aToken == token);
}

bool DomoticzTCP::StartHardwareProxy()
{
	http::server::CProxyClient *proxy;

	// we temporarily use the instance id as an identifier for this connection, meanwhile we get a token from the proxy
	// this means that we connect connect twice to the same server
	token = m_szIPAddress;
	proxy = m_webservers.GetProxyForClient(this);
	if (proxy) {
		proxy->ConnectToDomoticz(m_szIPAddress, m_username, m_password, this);
		return true;
	}
	return false;
}

bool DomoticzTCP::StopHardwareProxy()
{
	http::server::CProxyClient *proxy;

	proxy = m_webservers.GetProxyForClient(this);
	if (proxy) {
		proxy->DisconnectFromDomoticz(token, this);
	}
	b_ProxyConnected = false;
	return true;
}

bool DomoticzTCP::isConnectedProxy()
{
	return b_ProxyConnected;
}

void DomoticzTCP::writeProxy(const char *data, size_t size)
{
	/* send data to slave */
	http::server::CProxyClient *proxy;

	if (isConnectedProxy()) {
		proxy = m_webservers.GetProxyForClient(this);
		if (proxy) {
			proxy->WriteSlaveData(token, data, size);
		}
	}
}

void DomoticzTCP::FromProxy(const unsigned char *data, size_t datalen)
{
	/* data received from slave */
	boost::lock_guard<boost::mutex> l(readQueueMutex);
	onRFXMessage(data, datalen);
}

std::string DomoticzTCP::GetToken()
{
	return token;
}

void DomoticzTCP::Authenticated(const std::string &aToken, bool authenticated)
{
	b_ProxyConnected = authenticated;
	token = aToken;
	if (authenticated) {
		_log.Log(LOG_STATUS, "Domoticz TCP connected via Proxy.");
		sOnConnected(this);
	}
}
