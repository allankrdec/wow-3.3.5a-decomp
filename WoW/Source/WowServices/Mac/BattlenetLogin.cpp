// macOS counterpart of WowServices/Win64/BattlenetLogin.cpp: gethostbyname → getaddrinfo (IPv4, like the original).
//
// Battle.net login is out of scope for the Mac port and never reaches this function (Battlenet/Mac/Battlenet.cpp
// cannot load Battle.net, so there is no Tumor client to resolve for); it is ported anyway because it is portable.

#include <cstring>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "WoW/Source/WowServices/BattlenetLogin.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/Battlenet/Tumor/TumorManager.h"

// Win64: 0x008C67B0
uint32_t BattlenetApplication::Resolve(uint32_t token, const char* hostname) {
    if (!this->m_login || !g_tumorManager) {
        SErrDisplayAppFatalMessage("", "", 0);
    }

    Battlenet::SystemClient& client = g_tumorManager->m_tumor;

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags = AI_CANONNAME;

    addrinfo* host = nullptr;

    if (!getaddrinfo(hostname, nullptr, &hints, &host) && host) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in*>(host->ai_addr)->sin_addr);

        Battlenet::Client::DNSInfo info;
        info.m_addresses.SetCount(1);

        Battlenet::IP4::Address& address = info.m_addresses[0];
        address.m_value[0] = bytes[0];
        address.m_value[1] = bytes[1];
        address.m_value[2] = bytes[2];
        address.m_value[3] = bytes[3];

        info.m_name = host->ai_canonname ? host->ai_canonname : hostname;
        client.Resolved(token, info);
        freeaddrinfo(host);
        return 0;
    }

    if (host) {
        freeaddrinfo(host);
    }

    char name[256];
    const char* separator = SStrChr(hostname, ':');

    if (separator) {
        int32_t length = separator - hostname;

        if (length >= 255) {
            length = 255;
        }

        SStrCopy(name, hostname, length);
        name[length] = '\0';
    } else {
        SStrCopy(name, hostname, sizeof(name));
    }

    uint32_t ip = inet_addr(name);

    if (ip == INADDR_NONE) {
        BattlenetLogin::LogConnectionStatus("Failed to resolve %s", hostname);

        Battlenet::Client::DNSInfo info;
        info.m_name = "";
        client.Resolved(token, info);
        return 0;
    }

    Battlenet::Client::DNSInfo info;
    info.m_addresses.SetCount(1);

    Battlenet::IP4::Address& address = info.m_addresses[0];
    address.m_value[0] = ip;
    address.m_value[1] = ip >> 8;
    address.m_value[2] = ip >> 16;
    address.m_value[3] = ip >> 24;

    info.m_name = "";
    client.Resolved(token, info);
    return 0;
}
