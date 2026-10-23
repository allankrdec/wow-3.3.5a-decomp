// macOS counterpart of WowServices/Win64/WowConnection.cpp: the same connection state machine over BSD sockets.
//
// Winsock → POSIX: closesocket → close, ioctlsocket(FIONBIO) → fcntl(O_NONBLOCK), WSAGetLastError → errno
// (WSAEWOULDBLOCK → EWOULDBLOCK/EAGAIN, WSAEINTR → EINTR, and a non-blocking connect() in progress reports
// EINPROGRESS instead of WSAEWOULDBLOCK), gethostbyname → getaddrinfo (IPv4 only, like the original). Windows
// sockets never raise SIGPIPE; every socket created or accepted here gets SO_NOSIGPIPE so a send() to a closed peer
// fails with EPIPE and takes the normal disconnect path.

#include <alloca.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <new>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "WoW/Source/WowServices/WowConnection.h"
#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/WowServices/WDataStore.h"
#include "WoW/Source/WowServices/WowConnectionNet.h"

static bool MacSocketWouldBlock() {
    return errno == EWOULDBLOCK || errno == EAGAIN;
}

// ioctlsocket(sock, FIONBIO, &1) plus the per-socket options Windows gives for free.
static void MacSocketSetNonBlocking(int32_t sock) {
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
    fcntl(sock, F_SETFD, FD_CLOEXEC);

    int32_t noSigPipe = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
}

// Win64: 0x00466670
char* WowConnection::PeerAddrToStr(char* buffer, uint32_t size) {
    const sockaddr_in* peer = reinterpret_cast<const sockaddr_in*>(&this->m_peer.peerAddr);
    uint32_t address = peer->sin_addr.s_addr;
    SStrPrintf(buffer, size, "%d.%d.%d.%d:%u", address & 0xFF, (address >> 8) & 0xFF, (address >> 16) & 0xFF, address >> 24, ntohs(peer->sin_port));

    return buffer;
}

// Win64: 0x00466DE0
void WowConnection::CloseSocket(int32_t sock) {
    close(sock);

    if (sock >= 0) {
        WowConnection::s_idLock.Enter();
        WowConnection::s_idBits.m_bits.m_data[sock >> 5] &= ~(1 << (sock & 0x1F));
        WowConnection::s_idLock.Leave();
    }
}

// Win64: 0x00466F00
void WowConnection::DoStreamReads() {
    uint8_t buffer[4096];
    uint32_t startTime = OsGetAsyncTimeMs();
    int32_t bytes;

    while (true) {
        do {
            bytes = static_cast<int32_t>(recv(this->m_sock, buffer, sizeof(buffer), 0));
        } while (bytes < 0 && errno == EINTR);

        if (bytes <= 0) {
            break;
        }

        this->AcquireResponseRef();
        WowConnectionResponse* response = this->m_response;
        this->m_lock.Leave();

        if (response) {
            response->WCDataReady(this, OsGetAsyncTimeMs(), buffer, bytes);
        }

        this->m_lock.Enter();
        this->ReleaseResponseRef();

        if (this->m_connState == WOWC_DISCONNECTING || static_cast<int32_t>(OsGetAsyncTimeMs() - startTime) >= 5) {
            return;
        }
    }

    if (bytes < 0 && MacSocketWouldBlock()) {
        return;
    }

    this->AcquireResponseRef();
    WowConnectionResponse* response = this->m_response;

    WowConnection::s_network->Remove(this);
    this->CloseSocket(this->m_sock);
    this->SetState(WOWC_DISCONNECTED);

    int32_t sock = this->m_sock;
    this->m_lock.Leave();

    if (response && sock >= 0) {
        response->WCDisconnected(this, OsGetAsyncTimeMs(), &this->m_peer);
    }

    this->m_lock.Enter();
    this->m_sock = -1;
    this->ReleaseResponseRef();
}

// Win64: 0x00467270
WowConnection::WowConnection(int32_t sock, const NETADDR* addr, WowConnectionResponse* response) {
    this->Init(response, nullptr);
    socklen_t nameLength = sizeof(this->m_peer.peerAddr);
    getpeername(sock, reinterpret_cast<sockaddr*>(&this->m_peer.peerAddr), &nameLength);
    nameLength = sizeof(this->m_peer.selfAddr);
    getsockname(sock, reinterpret_cast<sockaddr*>(&this->m_peer.selfAddr), &nameLength);
    this->m_sock = sock;
    this->m_connState = WOWC_CONNECTED;
}

// Win64: 0x004675F0
WC_SEND_RESULT WowConnection::SendPacket(CDataStore* msg, int32_t headroom) {
    int32_t size = msg->m_size;
    void* data;
    msg->GetDataInSitu(data, size);
    WowConnection::s_sendBytes += size;

    WC_SEND_RESULT result = WC_SEND_ERROR;

    this->m_lock.Enter();

    if (size <= 0) {
        result = WC_SEND_ERROR;
    } else if (this->m_connState == WOWC_CONNECTED) {
        if (WowConnection::s_lagTestDelayMin || this->m_sendList.Head()) {
            WowConnection::SENDNODE* node = this->NewSendNodePacket(data, size, false);

            if (this->m_encrypt) {
                uint32_t length = this->m_uint375 - node->m_datasize + node->m_size;

                if (length > node->m_size) {
                    length = node->m_size;
                }

                SARC4ProcessBuffer(node->m_data, length, &this->m_sendKey, &this->m_sendKey);
            }

            this->m_sendList.LinkToTail(node);
            this->m_sendDepth++;
            this->m_sendBytes += node->m_size;
            WowConnection::s_network->PlatformChangeState(this, this->m_connState);

            if (this->m_sendDepth >= this->m_maxSendDepth) {
                this->SetState(WOWC_DISCONNECTING);
                result = WC_SEND_ERROR;

                if (this->m_response) {
                    this->m_response->NotifySendDepthExceeded(this);
                }

                char peer[80];
                this->PeerAddrToStr(peer, sizeof(peer));
                DebugLog("WowConnection: MaxSendDepth Reached! m_sendDepth=%d, m_maxSendDepth=%d, Dropping connection to %s", this->m_sendDepth, this->m_maxSendDepth, peer);
            } else {
                result = WC_SEND_QUEUED;
            }
        } else {
            WowConnection::SENDNODE* node;
            bool stackNode;

            if (msg->m_size >= 100000) {
                stackNode = false;
                node = this->NewSendNodePacket(data, size, false);
            } else {
                stackNode = true;

                if (headroom < 3) {
                    void* buffer = alloca(size + sizeof(WowConnection::SENDNODE) + 3);
                    node = new (buffer) WowConnection::SENDNODE(data, size, static_cast<uint8_t*>(buffer) + sizeof(WowConnection::SENDNODE), false);
                } else {
                    void* buffer = alloca(sizeof(WowConnection::SENDNODE));
                    node = new (buffer) WowConnection::SENDNODE(nullptr, size, static_cast<uint8_t*>(data), false);
                }
            }

            if (this->m_encrypt) {
                uint32_t length = this->m_uint375 - node->m_datasize + node->m_size;

                if (length > node->m_size) {
                    length = node->m_size;
                }

                this->Encrypt(node->m_data, length);
            }

            int32_t sent = static_cast<int32_t>(send(this->m_sock, node->m_data, node->m_size, 0));

            if (sent == static_cast<int32_t>(node->m_size)) {
                if (!stackNode) {
                    this->FreeSendNode(node);
                }

                result = WC_SEND_SENT;
            } else {
                bool wouldBlock = MacSocketWouldBlock();

                if (stackNode) {
                    node = this->NewSendNodeCopy(node, size);
                }

                if (sent > 0) {
                    node->m_offset += sent;
                    result = WC_SEND_QUEUED;
                } else if (!wouldBlock) {
                    this->SetState(WOWC_DISCONNECTING);
                    result = WC_SEND_ERROR;
                }

                this->m_sendList.LinkToTail(node);
                this->m_sendDepth++;
                this->m_sendBytes += node->m_size;

                if (this->m_sendDepth >= this->m_maxSendDepth) {
                    this->SetState(WOWC_DISCONNECTING);
                    result = WC_SEND_ERROR;

                    if (this->m_response) {
                        this->m_response->NotifySendDepthExceeded(this);
                    }

                    char peer[80];
                    this->PeerAddrToStr(peer, sizeof(peer));
                    DebugLog("WowConnection: MaxSendDepth Reached! m_sendDepth=%d, m_maxSendDepth=%d, Dropping connection to %s", this->m_sendDepth, this->m_maxSendDepth, peer);
                }

                WowConnection::s_network->PlatformChangeState(this, this->m_connState);
            }
        }
    }

    this->m_lock.Leave();

    return result;
}

// Win64: 0x00467990
WC_SEND_RESULT WowConnection::Send(uint8_t* data, int32_t size, bool raw) {
    WowConnection::s_sendBytes += size;

    WC_SEND_RESULT result = WC_SEND_ERROR;

    this->m_lock.Enter();

    if (!raw && this->m_encrypt) {
        int32_t length = this->m_uint375 + (size - 2 > 0x7FFF) + 2;

        if (length > size) {
            length = size;
        }

        SARC4ProcessBuffer(data, length, &this->m_sendKey, &this->m_sendKey);
    }

    if (size > 0 && this->m_connState == WOWC_CONNECTED) {
        if (!this->m_sendList.Head()) {
            int32_t sent = static_cast<int32_t>(send(this->m_sock, data, size, 0));

            if (sent == size) {
                this->m_lock.Leave();
                return WC_SEND_SENT;
            }

            if (sent < 0) {
                if (MacSocketWouldBlock()) {
                    sent = 0;
                } else if (this->m_sendDepth < this->m_maxSendDepth) {
                    this->SetState(WOWC_DISCONNECTING);
                    this->m_lock.Leave();
                    return WC_SEND_ERROR;
                }
            }

            WowConnection::SENDNODE* node = this->NewSendNodePacket(data + sent, size - sent, true);
            this->m_sendList.LinkToTail(node);
            result = WC_SEND_QUEUED;
            this->m_sendDepth++;
            this->m_sendBytes += node->m_size;
            WowConnection::s_network->PlatformChangeState(this, this->m_connState);
        } else {
            result = WC_SEND_QUEUED;

            WowConnection::SENDNODE* node = this->NewSendNodePacket(data, size, true);
            this->m_sendList.LinkToTail(node);
            this->m_sendDepth++;
            this->m_sendBytes += node->m_size;

            if (this->m_sendDepth >= this->m_maxSendDepth) {
                this->SetState(WOWC_DISCONNECTING);

                if (this->m_response) {
                    this->m_response->NotifySendDepthExceeded(this);
                }

                char peer[80];
                this->PeerAddrToStr(peer, sizeof(peer));
                DebugLog("WowConnection: MaxSendDepth Reached! m_sendDepth=%d, m_maxSendDepth=%d, Dropping connection to %s", this->m_sendDepth, this->m_maxSendDepth, peer);

                this->m_lock.Leave();
                return WC_SEND_ERROR;
            }

            WowConnection::s_network->PlatformChangeState(this, this->m_connState);
        }
    }

    this->m_lock.Leave();

    return result;
}

// Win64: 0x00467BA0
void WowConnection::DoMessageReads() {
    if (!this->m_readBuffer) {
        this->m_readBuffer = static_cast<uint8_t*>(SMemAlloc(1024, __FILE__, __LINE__, 0));
        this->m_readBufferSize = 1024;
        this->m_readBytes = 0;
    }

    uint32_t timeStamp = OsGetAsyncTimeMs();

    this->AcquireResponseRef();

    if (this->m_response) {
        this->m_response->NotifyAboutToDoReads();
    }

    this->ReleaseResponseRef();

    while (true) {
        WowConnection::SENDNODE* node = this->m_lagTestRecvList.Head();

        if (!node || static_cast<int32_t>(timeStamp - node->m_sendTime) < 0) {
            break;
        }

        CDataStore msg(node->m_data, node->m_size);

        this->AcquireResponseRef();
        WowConnectionResponse* response = this->m_response;

        if (response) {
            this->m_lock.Leave();
            response->WCMessageReady(this, timeStamp, &msg);
            this->m_lock.Enter();
        }

        this->ReleaseResponseRef();
        this->m_lagTestRecvList.UnlinkNode(node);
        WDataStore::FreeBuffer(node, node->m_datasize + sizeof(WowConnection::SENDNODE) + 3);
    }

    int32_t bytes;

    while (true) {
        int32_t headerSize = 2;
        int32_t packetSize = -1;

        if (this->m_readBytes >= 2) {
            uint8_t* buffer = this->m_readBuffer;

            if (buffer[0] & 0x80) {
                headerSize = 3;

                if (this->m_readBytes >= 3) {
                    packetSize = (buffer[2] | ((buffer[1] | ((buffer[0] & 0x7F) << 8)) << 8)) + 3;
                }
            } else {
                packetSize = (buffer[1] | ((buffer[0] & 0x7F) << 8)) + 2;
            }
        }

        if (this->m_readBytes >= this->m_readBufferSize) {
            this->m_readBuffer = static_cast<uint8_t*>(SMemReAlloc(this->m_readBuffer, 2 * this->m_readBufferSize, __FILE__, __LINE__, 0));
            this->m_readBufferSize *= 2;

            if (static_cast<uint32_t>(this->m_readBufferSize) > 32000000) {
                this->Disconnect();
                return;
            }
        }

        int32_t toRead;

        if (packetSize >= 0) {
            toRead = packetSize - this->m_readBytes;

            if (this->m_readBufferSize - this->m_readBytes < toRead) {
                toRead = this->m_readBufferSize - this->m_readBytes;
            }
        } else {
            toRead = headerSize - this->m_readBytes;
        }

        if (toRead <= 0) {
            bytes = 0;
        } else {
            do {
                bytes = static_cast<int32_t>(recv(this->m_sock, this->m_readBuffer + this->m_readBytes, toRead, 0));
            } while (bytes < 0 && errno == EINTR);

            if (bytes <= 0) {
                break;
            }
        }

        if (this->m_encrypt) {
            int32_t length = headerSize + this->m_uint376 - this->m_readBytes;

            if (length <= 0) {
                length = 0;
            }

            if (length >= bytes) {
                length = bytes;
            }

            SARC4ProcessBuffer(this->m_readBuffer + this->m_readBytes, length, &this->m_receiveKey, &this->m_receiveKey);
        }

        this->m_readBytes += bytes;

        if (packetSize >= 0 && this->m_readBytes >= packetSize) {
            CDataStore msg(this->m_readBuffer + headerSize, packetSize - headerSize);

            this->AcquireResponseRef();
            WowConnectionResponse* response = this->m_response;

            if (WowConnection::s_lagTestDelayMin) {
                WowConnection::SENDNODE* node = this->NewSendNodePacket(this->m_readBuffer + headerSize, packetSize - headerSize, true);
                this->m_lagTestRecvList.LinkToTail(node);
            } else if (response) {
                this->m_lock.Leave();
                response->WCMessageReady(this, timeStamp, &msg);
                this->m_lock.Enter();
            }

            this->m_readBytes = 0;
            this->ReleaseResponseRef();
        }

        if (bytes <= 0) {
            return;
        }
    }

    if (bytes < 0 && MacSocketWouldBlock()) {
        return;
    }

    this->AcquireResponseRef();
    WowConnectionResponse* response = this->m_response;

    WowConnection::s_network->Remove(this);
    this->CloseSocket(this->m_sock);
    this->SetState(WOWC_DISCONNECTED);

    int32_t sock = this->m_sock;
    this->m_lock.Leave();

    if (response && sock >= 0) {
        response->WCDisconnected(this, timeStamp, &this->m_peer);
    }

    this->m_lock.Enter();
    this->m_sock = -1;
    this->ReleaseResponseRef();
}

// Win64: 0x00468390
void WowConnection::CheckConnect() {
    int32_t error;
    socklen_t length = sizeof(error);

    if (getsockopt(this->m_sock, SOL_SOCKET, SO_ERROR, &error, &length)) {
        return;
    }

    if (error) {
        WowConnection::s_network->Remove(this);
        this->CloseSocket(this->m_sock);
        this->m_sock = -1;
        this->SetState(WOWC_DISCONNECTED);
        this->AddRef();
        this->AcquireResponseRef();

        WowConnectionResponse* response = this->m_response;
        this->m_lock.Leave();

        if (response) {
            response->WCCantConnect(this, OsGetAsyncTimeMs(), &this->m_peer);
        }
    } else {
        this->SetState(WOWC_CONNECTED);
        this->AddRef();
        this->AcquireResponseRef();

        WowConnectionResponse* response = this->m_response;
        this->m_lock.Leave();

        socklen_t nameLength = sizeof(this->m_peer.peerAddr);
        getpeername(this->m_sock, reinterpret_cast<sockaddr*>(&this->m_peer.peerAddr), &nameLength);
        nameLength = sizeof(this->m_peer.selfAddr);
        getsockname(this->m_sock, reinterpret_cast<sockaddr*>(&this->m_peer.selfAddr), &nameLength);

        if (response) {
            response->WCConnected(this, nullptr, OsGetAsyncTimeMs(), &this->m_peer);
        }
    }

    this->m_lock.Enter();
    this->ReleaseResponseRef();
    this->Release();
}

// Win64: 0x00468580
void WowConnection::CheckAccept() {
    for (int32_t i = 0; i < 1000; i++) {
        NETADDR addr;
        socklen_t addrLength = sizeof(addr);
        int32_t sock = accept(this->m_sock, reinterpret_cast<sockaddr*>(&addr), &addrLength);

        if (sock < 0) {
            break;
        }

        if (WowConnection::s_verifyAddr) {
            NETADDR peer;
            socklen_t peerLength = sizeof(peer);
            getpeername(sock, reinterpret_cast<sockaddr*>(&peer), &peerLength);

            if (!WowConnection::s_verifyAddr(&peer)) {
                close(sock);
                continue;
            }
        }

        WowConnection::MarkSocketId(sock);
        MacSocketSetNonBlocking(sock);

        WowConnection* inbound = new (__FILE__, __LINE__) WowConnection(sock, &addr, this->m_response);
        inbound->AddRef();
        this->AddRef();
        this->AcquireResponseRef();

        WowConnectionResponse* response = this->m_response;
        this->m_lock.Leave();

        if (response) {
            response->WCConnected(this, inbound, OsGetAsyncTimeMs(), &inbound->m_peer);
        }

        WowConnection::s_network->Add(inbound);
        WowConnection::s_network->PlatformChangeState(inbound, inbound->m_connState);
        inbound->Release();

        this->m_lock.Enter();
        this->ReleaseResponseRef();
        this->Release();
    }
}

// Win64: 0x00468790
void WowConnection::DoWrites() {
    this->AddRef();
    this->m_lock.Enter();

    bool disconnected = false;

    if (this->m_connState == WOWC_CONNECTING) {
        this->CheckConnect();
    } else if (this->m_sendList.Head()) {
        while (true) {
            WowConnection::SENDNODE* node = this->m_sendList.Head();

            if (!node) {
                break;
            }

            uint32_t now = OsGetAsyncTimeMs();

            if (WowConnection::s_lagTestDelayMin && static_cast<int32_t>(now - node->m_sendTime) < 0) {
                break;
            }

            uint32_t bytes = node->m_size - node->m_offset;

            if (bytes >= 0x400) {
                bytes = 0x400;
            }

            if (bytes) {
                int32_t sent = static_cast<int32_t>(send(this->m_sock, node->m_data + node->m_offset, bytes, 0));

                if (sent != static_cast<int32_t>(bytes)) {
                    if (sent > 0) {
                        node->m_offset += sent;
                        break;
                    }

                    if (sent >= 0 || MacSocketWouldBlock()) {
                        break;
                    }

                    disconnected = true;
                }
            }

            if (!disconnected) {
                if (bytes == node->m_size - node->m_offset) {
                    this->m_sendList.UnlinkNode(node);
                    this->m_sendDepth--;
                    this->m_sendBytes -= node->m_size;
                    WDataStore::FreeBuffer(node, node->m_datasize + sizeof(WowConnection::SENDNODE) + 3);
                } else {
                    node->m_offset += bytes;
                }
            }

            if (!this->m_sendList.Head() || disconnected) {
                break;
            }
        }
    } else {
        WowConnection::s_network->PlatformChangeState(this, this->m_connState);
    }

    WowConnectionResponse* response = nullptr;
    int32_t sock = -1;
    bool writeReady = false;

    if (disconnected) {
        this->AcquireResponseRef();
        response = this->m_response;
        WowConnection::s_network->Remove(this);
        this->CloseSocket(this->m_sock);
        sock = this->m_sock;
        this->SetState(WOWC_DISCONNECTED);
    } else if (this->m_notifyWriteReady && !this->m_sendList.Head()) {
        writeReady = true;
        this->m_notifyWriteReady = 0;
        this->AcquireResponseRef();
        response = this->m_response;
    }

    this->m_lock.Leave();

    if (disconnected) {
        if (response && sock >= 0) {
            response->WCDisconnected(this, OsGetAsyncTimeMs(), &this->m_peer);
        }

        this->m_lock.Enter();
        this->m_sock = -1;
        this->ReleaseResponseRef();
        this->m_lock.Leave();
    } else if (writeReady) {
        if (response) {
            response->WCWriteReady(this);
        }

        this->m_lock.Enter();
        this->ReleaseResponseRef();
        this->m_lock.Leave();
    }

    this->Release();
}

// Win64: 0x00468BA0
void WowConnection::StartConnect() {
    if (this->m_sock >= 0) {
        if (this->m_netLink.IsLinked()) {
            WowConnection::s_network->Remove(this);
        }

        this->CloseSocket(this->m_sock);
        this->m_sock = -1;
    }

    this->m_lock.Enter();

    int32_t sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        SErrDisplayErrorFmt(0x85100000, __FILE__, __LINE__, 0, 1, "socket() returned %d, reason: %s", sock, "can't create socket (No network connection available?)");
    }

    WowConnection::MarkSocketId(sock);
    this->m_sock = sock;

    if (sock >= 0) {
        MacSocketSetNonBlocking(sock);

        sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        address.sin_port = htons(this->m_connectPort);
        address.sin_addr.s_addr = this->m_connectAddress;

        if (!this->m_netLink.IsLinked()) {
            WowConnection::s_network->Add(this);
        }

        this->m_readBytes = 0;
        this->SetState(WOWC_CONNECTING);

        // Windows reports a pending non-blocking connect as WSAEWOULDBLOCK; BSD sockets report EINPROGRESS (or EINTR,
        // after which the connect still completes asynchronously). Either way the network thread waits for
        // writability and CheckConnect reads SO_ERROR, so a refused connection still ends in WCCantConnect.
        if (connect(this->m_sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) >= 0 || errno == EINPROGRESS || errno == EINTR || MacSocketWouldBlock()) {
            this->m_lock.Leave();
            return;
        }

        // Winsock reports a failed connect through FD_CONNECT, so on Windows the response always hears about it.
        // macOS can refuse at once (EHOSTUNREACH when the app has no Local Network access, ENETUNREACH, ...):
        // report that as WCCantConnect too, instead of leaving the caller waiting on a connection that never
        // started ("Connecting" forever).
        int32_t error = errno;
        std::fprintf(stderr, "WowConnection: connect() to %s:%u failed: %s\n", inet_ntoa(address.sin_addr), this->m_connectPort, std::strerror(error));

        WowConnection::s_network->Remove(this);
        this->CloseSocket(this->m_sock);
        this->m_sock = -1;
        this->SetState(WOWC_DISCONNECTED);
        this->AddRef();
        this->AcquireResponseRef();

        WowConnectionResponse* response = this->m_response;
        this->m_lock.Leave();

        if (response) {
            response->WCCantConnect(this, OsGetAsyncTimeMs(), &this->m_peer);
        }

        this->m_lock.Enter();
        this->ReleaseResponseRef();
        this->m_lock.Leave();
        this->Release();
        return;
    }

    this->SetState(WOWC_ERROR);
    this->m_lock.Leave();
}

// Win64: 0x00468D20
bool WowConnection::Connect(const char* host, uint16_t port, int32_t timeout) {
    uint32_t address = inet_addr(host);

    if (address == INADDR_NONE || !address) {
        addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* entry = nullptr;

        if (!getaddrinfo(host, nullptr, &hints, &entry) && entry) {
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in*>(entry->ai_addr)->sin_addr);
            this->m_connectAddress = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
        } else {
            this->m_connectAddress = 0;
        }

        if (entry) {
            freeaddrinfo(entry);
        }
    } else {
        this->m_connectAddress = address;
    }

    this->m_connectPort = port;
    this->StartConnect();

    return true;
}
