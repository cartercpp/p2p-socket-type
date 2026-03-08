#include "p2p_socket.hpp"
#include <string>
#include <string_view>
#include <map>
#include <set>
#include <queue>
#include <vector>
#include <utility>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstdint>
#include <cstddef>
#include "ip_type.hpp"

#define NOMINMAX
#pragma comment(lib, "ws2_32.lib")
#include <WinSock2.h>
#include <WS2tcpip.h>

p2p_socket::p2p_socket(IP_TYPE ipType, std::uint16_t portNumber)
	: m_ipType{ ipType }
{
	m_wsaSuccess = WSAStartup(MAKEWORD(2, 2), &m_wsa) == 0;
	if (!m_wsaSuccess)
		throw std::runtime_error{ "Failed to initialize winsock library" };

	addrinfo* localAddress = ConfigureAddress(nullptr, ipType, portNumber, true);
	m_socket = socket(localAddress->ai_family, localAddress->ai_socktype,
					localAddress->ai_protocol);
	if (m_socket == INVALID_SOCKET)
	{
		freeaddrinfo(localAddress);
		throw std::runtime_error{ "Failed to create socket" };
	}

	const bool bindSuccess = bind(m_socket, localAddress->ai_addr, localAddress->ai_addrlen) == 0;
	freeaddrinfo(localAddress);
	if (!bindSuccess)
	{
		closesocket(m_socket);
		throw std::runtime_error{ "Failed to bind socket to local machine's socket address" };
	}

	const bool listenSuccess = listen(m_socket, SOMAXCONN) == 0;
	if (!listenSuccess)
	{
		closesocket(m_socket);
		throw std::runtime_error{ "Failed to listen for incoming connections" };
	}

	m_acceptConnThread = std::jthread{ &p2p_socket::AcceptConnFunc, this, m_src.get_token() };
	m_closeConnThread = std::jthread{ &p2p_socket::CloseConnFunc, this, m_src.get_token() };
}

p2p_socket::~p2p_socket()
{
	m_src.request_stop();
	m_cv.notify_one();
	shutdown(m_socket, SD_BOTH);

	{
		std::lock_guard<std::mutex> lck{ m_mtx };
		for (SOCKET peerSocket : m_peerSockets)
			shutdown(peerSocket, SD_BOTH);
	}

	if (m_acceptConnThread.joinable())
		m_acceptConnThread.join();

	if (m_closeConnThread.joinable())
		m_closeConnThread.join();

	for (std::jthread& thrRef : m_peerThreads)
		if (thrRef.joinable())
			thrRef.join();

	if (m_socket != INVALID_SOCKET)
		closesocket(m_socket);

	if (m_wsaSuccess)
		WSACleanup();
}

bool p2p_socket::try_connect(std::string_view peerIpAddress, std::uint16_t portNumber)
{
	try
	{
		addrinfo* peerAddress = ConfigureAddress(peerIpAddress.data(), m_ipType, portNumber, true);
		SOCKET peerSocket = socket(peerAddress->ai_family, peerAddress->ai_socktype,
									peerAddress->ai_protocol);
		if (peerSocket == INVALID_SOCKET)
		{
			freeaddrinfo(peerAddress);
			return false;
		}

		const bool connectSuccess
			= connect(peerSocket, peerAddress->ai_addr, peerAddress->ai_addrlen) == 0;
		freeaddrinfo(peerAddress);
		
		if (!connectSuccess)
			return false;

		std::lock_guard<std::mutex> lck{ m_mtx };
		m_peerSockets.push_back(peerSocket);
		m_peerThreads.emplace_back(&p2p_socket::ListenFunc, this, m_src.get_token(),
									peerSocket, std::string{ peerIpAddress });

		return true;
	}
	catch (...)
	{
		return false;
	}
}

std::set<std::string> p2p_socket::connections() const
{
	std::lock_guard<std::mutex> lck{ m_mtx };
	return m_activeConnections;
}

std::map<std::string, std::vector<std::string>> p2p_socket::received() const
{
	std::lock_guard<std::mutex> lck{ m_mtx };
	return m_ipToReceived;
}

void p2p_socket::clear_received()
{
	std::lock_guard<std::mutex> lck{ m_mtx };
	m_ipToReceived.clear();
}

void p2p_socket::send_all(std::string_view message) const
{
	std::lock_guard<std::mutex> lck{ m_mtx };
	for (SOCKET peerSocket : m_peerSockets)
	{
		try
		{
			SendMessageToSocket(peerSocket, message);
		}
		catch (...)
		{
		}
	}
}

addrinfo* p2p_socket::ConfigureAddress(const char* ipAddress, IP_TYPE ipType,
										std::uint16_t portNumber, bool isServer)
{
	addrinfo* address = nullptr;

	addrinfo hints{};
	switch (ipType)
	{
	case IP_TYPE::IPV4:
		hints.ai_family = AF_INET;
		break;
	case IP_TYPE::IPV6:
		hints.ai_family = AF_INET6;
		break;
	}
	hints.ai_socktype = SOCK_STREAM;
	if (isServer)
		hints.ai_flags = AI_PASSIVE;

	const std::string portStr{ std::to_string(portNumber) };
	const bool getaddrinfoSuccess = getaddrinfo(ipAddress, portStr.data(), &hints, &address) == 0;
	if (!getaddrinfoSuccess)
		throw std::runtime_error{ "Failed to configure requested address" };

	return address;
}

void p2p_socket::SendMessageToSocket(SOCKET socket, std::string_view message)
{
	std::size_t index = 0;

	while (index < message.size())
	{
		const int bytesSent = send(
			socket,
			message.data() + index,
			std::min(s_bufferSize, static_cast<int>(message.size() - index)),
			0
		);

		if (bytesSent <= 0)
			throw std::runtime_error{ "Failed to send message to socket" };

		index += bytesSent;
	}

	if (send(socket, &s_endFlag, 1, 0) != 1)
		throw std::runtime_error{ "Failed to send message to socket" };
}

void p2p_socket::AcceptConnFunc(std::stop_token st)
{
	while (!st.stop_requested())
	{
		fd_set readFds{};
		FD_ZERO(&readFds);
		FD_SET(m_socket, &readFds);

		const timeval tv{ .tv_sec = 0, .tv_usec = 250'000 };
		const int ready = select(0, &readFds, nullptr, nullptr, &tv);

		if ((ready > 0) && FD_ISSET(m_socket, &readFds))
		{
			sockaddr_storage clientAddress{};
			int clientSize = static_cast<int>(sizeof(clientAddress));

			SOCKET peerSocket
				= accept(m_socket, reinterpret_cast<sockaddr*>(&clientAddress), &clientSize);
			if (peerSocket == INVALID_SOCKET)
				continue;

			char host[NI_MAXHOST];
			const bool getnameinfoSuccess = getnameinfo(
				reinterpret_cast<sockaddr*>(&clientAddress), clientSize,
				host, sizeof(host),
				nullptr, 0,
				NI_NUMERICHOST
			) == 0;
			if (!getnameinfoSuccess)
			{
				closesocket(peerSocket);
				continue;
			}

			std::lock_guard<std::mutex> lck{ m_mtx };
			m_activeConnections.emplace(host);
			m_peerSockets.push_back(peerSocket);
			m_peerThreads.emplace_back(
				&p2p_socket::ListenFunc, this, m_src.get_token(), peerSocket, std::string{host});
		}
	}
}

void p2p_socket::CloseConnFunc(std::stop_token st)
{
	while (!st.stop_requested())
	{
		std::unique_lock<std::mutex> lck{ m_mtx };
		m_cv.wait(lck,
				[this, &st]() {return !m_closeConnectionQueue.empty() || st.stop_requested(); });

		while (!m_closeConnectionQueue.empty())
		{
			auto& [peerSocket, peerIpAddress] = m_closeConnectionQueue.front();
			
			closesocket(peerSocket);
			m_activeConnections.erase(peerIpAddress);

			m_closeConnectionQueue.pop();
		}
	}
}

void p2p_socket::ListenFunc(std::stop_token st, SOCKET peerSocket, std::string peerIpAddress)
{
	std::string receivedData;
	char buffer[s_bufferSize];

	while (!st.stop_requested())
	{
		const int bytesReceived = recv(peerSocket, buffer, s_bufferSize, 0);
		if (bytesReceived <= 0)
			break;
		receivedData.append(buffer, bytesReceived);

		std::size_t size;
		while ((size = receivedData.find(s_endFlag)) != std::string::npos)
		{
			const std::string_view message(receivedData.data(), size);
			{
				std::lock_guard<std::mutex> lck{ m_mtx };
				m_ipToReceived[peerIpAddress].emplace_back(message);
			}
			receivedData.erase(0, size + 1);
		}
	}

	{
		std::lock_guard<std::mutex> lck{ m_mtx };
		m_closeConnectionQueue.emplace(peerSocket, peerIpAddress);
	}
	m_cv.notify_one();
}
