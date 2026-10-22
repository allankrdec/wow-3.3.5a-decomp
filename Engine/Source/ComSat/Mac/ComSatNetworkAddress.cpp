// macOS counterpart of ComSat/Win64/ComSatNetworkAddress.cpp. Same code: inet_addr/htons are POSIX.

#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include "Engine/Source/ComSat/ComSatNetwork.h"

// Win64: 0x00885C40
void ComSat::Network::Address::SetPort(int32_t port) {
    this->m_port = htons(static_cast<uint16_t>(port));
}

// Win64: 0x00885C60
int32_t ComSat::Network::Address::GetPort() const {
    return ntohs(this->m_port);
}

// Win64: 0x00885CD0
void ComSat::Network::Address::SetAddress(const char* address) {
    const char* colon = strchr(address, ':');
    if (colon) {
        if (strlen(address) < 128) {
            char host[128];
            strcpy(host, address);
            host[colon - address] = '\0';
            this->m_ip = inet_addr(host);
            this->m_port = htons(static_cast<uint16_t>(strtol(colon + 1, nullptr, 10)));
        }
    } else {
        this->m_ip = inet_addr(address);
    }
}
