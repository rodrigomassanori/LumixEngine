#pragma once


#ifndef DISABLE_NETWORK


class Socket
{
	public:
		Socket();
		~Socket();

		static bool init();

		bool create(unsigned short port);
		Socket* accept();
		bool send(const void* data, int size);
		int receive(void* data, int size);
		bool receiveAllBytes(void* data, int size);
		bool canReceive();

	private:
		struct SocketImpl* m_implmentation;
};


#endif // DISABLE_NETWORK