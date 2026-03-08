#include <iostream>
#include <string>
#include <format>
#include <stop_token>
#include <thread>
#include <chrono>
#include "p2p_socket.hpp"

int main()
{
	std::this_thread::sleep_for(std::chrono::seconds(4));
	p2p_socket sock{ IP_TYPE::IPV4, 8080 };
	sock.try_connect("127.0.0.1", 8081);

	std::jthread pollingThread{ [&](std::stop_token st) {
		while (!st.stop_requested())
		{
			const auto ipToReceived = sock.received();
			sock.clear_received();

			for (const auto& [ip, messages] : ipToReceived)
				for (const auto& message : messages)
					std::cout << std::format("{}: \"{}\"\n", ip, message);

			std::this_thread::sleep_for(std::chrono::microseconds(250'000));
		}
	} };

	while (true)
	{
		std::string input;
		std::getline(std::cin, input);

		if (input.empty())
			break;
		
		sock.send_all(input);
	}

	return 0;
}
