// macOS counterpart of WowServices/Win64/WowConnectionNet.cpp.
//
// Windows waits on one WSAEventSelect event per socket plus m_event (set to wake the network thread). Here the
// network thread poll()s the sockets directly and m_event is a self-pipe: writing a byte wakes poll(). The
// readiness rules are the same: connecting sockets wait for connect/close, listening ones for accept, connected
// ones for read/close plus write while something is queued.

#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "WoW/Source/WowServices/WowConnectionNet.h"

struct MacNetWake {
    int32_t m_read;
    int32_t m_write;
};

static void MacNetWakeSignal(void* event) {
    MacNetWake* wake = static_cast<MacNetWake*>(event);
    if (!wake) {
        return;
    }

    char byte = 0;
    write(wake->m_write, &byte, 1);
}

static void MacNetWakeDrain(MacNetWake* wake) {
    char buffer[64];
    while (read(wake->m_read, buffer, sizeof(buffer)) > 0) {
    }
}

// Win64: 0x004697E0
void WowConnectionNet::PlatformChangeState(WowConnection* connection, WOW_CONN_STATE state) {
    MacNetWakeSignal(this->m_event);
}

// Win64: 0x00469860
void WowConnectionNet::PlatformRun() {
    this->m_networkStarted = true;

    int32_t timeout = 500;
    MacNetWake* wake = static_cast<MacNetWake*>(this->m_event);

    if (this->m_stop || !wake) {
        return;
    }

    do {
        const uint32_t maxConnections = 64;
        WowConnection* connections[maxConnections];
        pollfd fds[maxConnections];

        this->m_connectionsLock.Enter();

        connections[0] = nullptr;
        fds[0].fd = wake->m_read;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        uint32_t count = 1;

        for (WowConnection* connection = this->m_connections.Head(); connection; connection = this->m_connections.Next(connection)) {
            if (connection->m_serviceCount) {
                continue;
            }

            int16_t events = 0;

            switch (connection->m_connState) {
                case WOWC_CONNECTING:
                    events = POLLOUT;
                    break;

                case WOWC_LISTENING:
                    events = POLLIN;
                    break;

                case WOWC_CONNECTED:
                    events = POLLIN | (connection->m_sendList.Head() || connection->m_notifyWriteReady ? POLLOUT : 0);
                    break;

                case WOWC_DISCONNECTING:
                    timeout = 0;
                    break;

                default:
                    continue;
            }

            if (count < maxConnections) {
                connection->m_refCount.Increment();
                fds[count].fd = connection->m_connState == WOWC_DISCONNECTING ? -1 : connection->m_sock;
                fds[count].events = events;
                fds[count].revents = 0;
                connections[count] = connection;
                count++;

                if (connection->m_lagTestRecvList.Head()) {
                    timeout = 0;
                }
            }
        }

        this->m_connectionsLock.Leave();

        int32_t result = poll(fds, count, timeout);

        if (result >= 0) {
            if (fds[0].revents & POLLIN) {
                MacNetWakeDrain(wake);
            }

            for (uint32_t i = 1; i < count; i++) {
                WowConnection* connection = connections[i];
                int16_t revents = fds[i].revents;
                uint32_t flags = 0;

                if (revents & (POLLIN | POLLHUP | POLLERR)) {
                    flags = 2;
                }

                if (revents & (POLLOUT | POLLHUP | POLLERR)) {
                    flags |= 1;
                }

                if (connection->m_connState == WOWC_DISCONNECTING) {
                    flags |= 8;
                }

                if (WowConnection::s_lagTestDelayMin || WowConnection::s_lagTestDelayMax) {
                    flags |= 3;
                    timeout = 10;
                } else {
                    timeout = 500;
                }

                if (flags) {
                    this->SignalWorker(connection, flags);
                }
            }
        }

        for (uint32_t i = 1; i < count; i++) {
            connections[i]->Release();
        }
    } while (!this->m_stop);
}

// Win64: 0x004696E0
void WowConnectionNet::PlatformInit(bool useEngine) {
    int32_t fds[2];
    if (pipe(fds)) {
        return;
    }

    for (int32_t fd : fds) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    MacNetWake* wake = new MacNetWake;
    wake->m_read = fds[0];
    wake->m_write = fds[1];
    this->m_event = wake;
}

// Win64: 0x00469740
void WowConnectionNet::PlatformDestruct(WowConnection* connection) {
    connection->m_event = nullptr;
}

// Win64: 0x00469770
void WowConnectionNet::PlatformWorkerReady() {
    MacNetWakeSignal(this->m_event);
}

// Win64: 0x00469780
void WowConnectionNet::PlatformAdd(WowConnection* connection) {
    int32_t noDelay = 1;
    setsockopt(connection->m_sock, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

    // Windows sockets do not raise SIGPIPE; a write to a closed connection must fail with EPIPE instead.
    int32_t noSigPipe = 1;
    setsockopt(connection->m_sock, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));

    MacNetWakeSignal(this->m_event);
}

// Win64: 0x004697D0
void WowConnectionNet::PlatformRemove(WowConnection* connection) {
    MacNetWakeSignal(this->m_event);
}

// Win64: 0x005EEB70
void WowConnectionNet::PlatformStop() {
    MacNetWakeSignal(this->m_event);
}

// Win64: 0x00469730
void WowConnectionNet::PlatformDestroy() {
    MacNetWake* wake = static_cast<MacNetWake*>(this->m_event);
    if (!wake) {
        return;
    }

    close(wake->m_read);
    close(wake->m_write);
    delete wake;
    this->m_event = nullptr;
}
