// macOS counterpart of Os/Win64/OsTcp.cpp.
//
// The Win64 file is the IOCP-based OsNet/NETCONN engine (TCPNET, TCPCONN, AcceptEx, overlapped WSARecv/WSASend).
// In the client nothing creates a TCPNET: the only OsTcp entry points referenced outside Win32/Win64 are the NETADDR
// helpers below (Grunt, GruntLogin, BattlenetLogin) and OsNetPump (Event/EvtSched.cpp, only on the s_netServer
// path). Login and world traffic go through WowConnection/WowConnectionNet instead. So the Mac build keeps just
// these four functions; OsNetPump behaves like Win64 with no TCPNET (TCPNET::s_pnet == nullptr) and sleeps.
// TODO_MAC: port the TCPNET engine (on kqueue) only if some Mac code path ever starts creating one.
//
// NETADDR holds a sockaddr_in. The BSD sockaddr_in starts with sin_len/sin_family (one byte each) instead of the
// 16-bit Windows sin_family, so OsNetAddrMake fills sin_len too; the result is what getpeername/getsockname write
// into WowConnection::m_peer, and what bind/connect accept.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "Engine/Source/Os/OsTcp.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

static_assert(sizeof(sockaddr_in) == sizeof(NETADDR), "NETADDR must hold a sockaddr_in");

// Win64: 0x0086DBF0
void OsNetAddrMake(uint32_t address, uint16_t port, NETADDR* addr) {
    sockaddr_in* in = reinterpret_cast<sockaddr_in*>(addr);
    in->sin_len = sizeof(sockaddr_in);
    in->sin_family = AF_INET;
    in->sin_port = htons(port);
    in->sin_addr.s_addr = address;
    memset(in->sin_zero, 0, sizeof(in->sin_zero));
}

// Win64: 0x0086DC20
uint32_t OsNetAddrGetAddress(const NETADDR* addr, uint16_t* port) {
    const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(addr);

    if (port) {
        *port = ntohs(in->sin_port);
    }

    return in->sin_addr.s_addr;
}

// Win64: 0x0086DC50
const char* OsNetAddrToStr(const NETADDR* addr, char* buffer, uint32_t size) {
    const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(addr);
    char text[INET_ADDRSTRLEN];

    if (!inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text))) {
        text[0] = '\0';
    }

    SStrPrintf(buffer, size, "%s:%u", text, ntohs(in->sin_port));
    return buffer;
}

// Win64: 0x0086F1C0
void OsNetPump(uint32_t timeout) {
    // No TCPNET on the Mac (see the top of this file): Win64's `Sleep(timeout)` branch.
    OsSleep(timeout);
}
