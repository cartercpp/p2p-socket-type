#pragma once

#include <string>
#include <string_view>
#include <map>
#include <set>
#include <queue>
#include <vector>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstdint>
#include "ip_type.hpp"

#define NOMINMAX
#pragma comment(lib, "ws2_32.lib")
#include <WinSock2.h>
#include <WS2tcpip.h>

class p2p_socket
{
public:

	//CONSTRUCTORS & STUFF

	explicit p2p_socket(IP_TYPE, std::uint16_t);
	~p2p_socket();

	//METHODS & STUFF

	bool try_connect(std::string_view, std::uint16_t);
	std::set<std::string> connections() const;
	std::map<std::string, std::vector<std::string>> received() const;
	void clear_received();
	void send_all(std::string_view) const;

private:

	static constexpr int s_bufferSize = 1024;
	static constexpr char s_endFlag = '\0';
	static void SendMessageToSocket(SOCKET, std::string_view);
	static addrinfo* ConfigureAddress(const char*, IP_TYPE, std::uint16_t, bool);

	void AcceptConnFunc(std::stop_token);
	void CloseConnFunc(std::stop_token);
	void ListenFunc(std::stop_token, SOCKET, std::string);

	WSAData m_wsa;
	bool m_wsaSuccess;
	IP_TYPE m_ipType;
	SOCKET m_socket;

	mutable std::mutex m_mtx;
	std::stop_source m_src;
	std::condition_variable m_cv;

	std::set<std::string> m_activeConnections;
	std::map<std::string, std::vector<std::string>> m_ipToReceived;
	std::queue<std::pair<SOCKET, std::string>> m_closeConnectionQueue;

	std::vector<SOCKET> m_peerSockets;
	std::vector<std::jthread> m_peerThreads;
	std::jthread m_acceptConnThread, m_closeConnThread;
};
