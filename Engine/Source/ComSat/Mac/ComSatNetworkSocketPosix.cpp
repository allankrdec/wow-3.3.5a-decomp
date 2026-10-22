// macOS counterpart of ComSat/Win64/ComSatNetworkSocketPosix.cpp (voice chat UDP socket).
//
// ComSat::Network::Address mirrors the Windows sockaddr_in from m_family on (16-bit family, port, address, zero).
// The BSD sockaddr_in starts with an 8-bit sin_len and an 8-bit sin_family, so the Address is converted to and from
// a real sockaddr_in around bind/sendto/recvfrom instead of being cast. The SIO_UDP_CONNRESET ioctl
// (DisableConnectionReset) has no equivalent and is not needed: BSD UDP sockets that are not connect()ed do not
// report ICMP port-unreachable on recvfrom.

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "Engine/Source/ComSat/ComSatNetwork.h"

static void AddressToSockaddr(const ComSat::Network::Address& address, sockaddr_in& in) {
    memset(&in, 0, sizeof(in));
    in.sin_len = sizeof(in);
    in.sin_family = static_cast<sa_family_t>(address.m_family);
    in.sin_port = address.m_port;
    in.sin_addr.s_addr = address.m_ip;
}

static int32_t SocketFd(const ComSat::Network::SocketPosix* socket) {
    return static_cast<int32_t>(static_cast<intptr_t>(socket->m_socket));
}

// Win64: 0x008888F0
ComSat::Network::SocketPosix::SocketPosix() {
    this->m_socket = static_cast<uintptr_t>(static_cast<intptr_t>(socket(AF_INET, SOCK_DGRAM, 0)));
    int32_t sock = SocketFd(this);
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
    fcntl(sock, F_SETFD, FD_CLOEXEC);
    int32_t value = 0x8000;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
}

// Win64: 0x00888960
ComSat::Network::SocketPosix::~SocketPosix() {
    close(SocketFd(this));
}

// Win64: 0x00888800
bool ComSat::Network::SocketPosix::Bind(int32_t port) {
    ComSat::Network::Address address;
    address.SetPort(port);
    int32_t reuse = 1;
    setsockopt(SocketFd(this), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in in;
    AddressToSockaddr(address, in);
    return bind(SocketFd(this), reinterpret_cast<const sockaddr*>(&in), sizeof(in)) != -1;
}

// Win64: 0x00888860
int32_t ComSat::Network::SocketPosix::SendTo(const ComSat::Network::Address& address, const uint8_t* data, uint32_t size) {
    sockaddr_in in;
    AddressToSockaddr(address, in);
    return static_cast<int32_t>(sendto(SocketFd(this), data, size, 0, reinterpret_cast<const sockaddr*>(&in), sizeof(in)));
}

// Win64: 0x00888890
bool ComSat::Network::SocketPosix::RecvFrom(ComSat::Network::Address& address, uint8_t* data, uint32_t& size) {
    memset(data, 0, size);
    sockaddr_in in;
    memset(&in, 0, sizeof(in));
    socklen_t length = sizeof(in);
    int32_t received = static_cast<int32_t>(recvfrom(SocketFd(this), data, size, 0, reinterpret_cast<sockaddr*>(&in), &length));
    if (received == -1) {
        return false;
    }
    if (!received) {
        return false;
    }
    // Same fields recvfrom would have written into the Windows layout (m_length is the sockaddr_in size, 16).
    address.m_length = 16;
    address.m_family = in.sin_family;
    address.m_port = in.sin_port;
    address.m_ip = in.sin_addr.s_addr;
    memset(address.m_zero, 0, sizeof(address.m_zero));
    size = received;
    return true;
}
