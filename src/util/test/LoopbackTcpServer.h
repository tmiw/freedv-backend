//=========================================================================
// Name:            LoopbackTcpServer.h
// Purpose:         Minimal blocking TCP server on 127.0.0.1 for unit tests
//                  that need a real peer for TcpConnectionHandler.
//
// License:
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.1,
//  as published by the Free Software Foundation.  This program is
//  distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
//  License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, see <http://www.gnu.org/licenses/>.
//
//=========================================================================

#ifndef LOOPBACK_TCP_SERVER_H
#define LOOPBACK_TCP_SERVER_H

#include <cstring>
#include <string>

#include "TestSocketCompat.h"

// Listens on an ephemeral port on 127.0.0.1 and accepts one peer at a time.
// All waits are bounded so a misbehaving client fails the test rather than
// hanging it.
class LoopbackTcpServer
{
public:
    // Listens on 127.0.0.1, or on ::1 if family is AF_INET6 (valid() is
    // false if the machine has no IPv6 loopback).
    explicit LoopbackTcpServer(int family = AF_INET)
        : listenFd_(-1)
        , peerFd_(-1)
        , port_(-1)
    {
        listenFd_ = testOpenSocket(family, SOCK_STREAM);
        if (listenFd_ < 0)
        {
            return;
        }

        int on = 1;
        testSetSockOpt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        struct sockaddr_storage addr;
        socklen_t addrLen;
        memset(&addr, 0, sizeof(addr));
        if (family == AF_INET6)
        {
            auto addr6 = (struct sockaddr_in6*)&addr;
            addr6->sin6_family = AF_INET6;
            addr6->sin6_addr = in6addr_loopback;
            addrLen = sizeof(struct sockaddr_in6);
        }
        else
        {
            auto addr4 = (struct sockaddr_in*)&addr;
            addr4->sin_family = AF_INET;
            addr4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addrLen = sizeof(struct sockaddr_in);
        }

        if (bind(listenFd_, (struct sockaddr*)&addr, addrLen) < 0 ||
            listen(listenFd_, 4) < 0 ||
            getsockname(listenFd_, (struct sockaddr*)&addr, &addrLen) < 0)
        {
            testCloseSocket(listenFd_);
            listenFd_ = -1;
            return;
        }
        port_ = ntohs(family == AF_INET6 ? ((struct sockaddr_in6*)&addr)->sin6_port
                                         : ((struct sockaddr_in*)&addr)->sin_port);
    }

    ~LoopbackTcpServer()
    {
        closePeer();
        if (listenFd_ >= 0)
        {
            testCloseSocket(listenFd_);
        }
    }

    LoopbackTcpServer(const LoopbackTcpServer&) = delete;
    LoopbackTcpServer& operator=(const LoopbackTcpServer&) = delete;

    bool valid() const { return listenFd_ >= 0; }
    int port() const { return port_; }
    int peerFd() const { return peerFd_; }

    // Waits for a client to connect. Replaces any existing peer.
    bool accept(int timeoutMs)
    {
        if (!waitReadable_(listenFd_, timeoutMs))
        {
            return false;
        }
        closePeer();
        peerFd_ = testAccept(listenFd_);
        return peerFd_ >= 0;
    }

    void closePeer()
    {
        if (peerFd_ >= 0)
        {
            testCloseSocket(peerFd_);
            peerFd_ = -1;
        }
    }

    bool sendAll(const void* data, size_t length)
    {
        const char* ptr = static_cast<const char*>(data);
        while (length > 0)
        {
            ssize_t written = ::send(peerFd_, ptr, length, 0);
            if (written <= 0)
            {
                return false;
            }
            ptr += written;
            length -= written;
        }
        return true;
    }

    bool sendAll(const std::string& data) { return sendAll(data.data(), data.size()); }

    // Reads exactly length bytes, or fails on timeout/EOF.
    bool recvExact(void* data, size_t length, int timeoutMs)
    {
        char* ptr = static_cast<char*>(data);
        while (length > 0)
        {
            if (!waitReadable_(peerFd_, timeoutMs))
            {
                return false;
            }
            ssize_t numRead = ::recv(peerFd_, ptr, length, 0);
            if (numRead <= 0)
            {
                return false;
            }
            ptr += numRead;
            length -= numRead;
        }
        return true;
    }

    // Returns true if the peer closed its end (recv() == 0) within the timeout.
    // Discards any data that arrives first.
    bool waitForPeerClose(int timeoutMs)
    {
        char buf[1024];
        while (waitReadable_(peerFd_, timeoutMs))
        {
            ssize_t numRead = ::recv(peerFd_, buf, sizeof(buf), 0);
            if (numRead <= 0)
            {
                return true;
            }
        }
        return false;
    }

    // Returns an unused port: binds an ephemeral port and releases it again,
    // so a connection attempt to it is refused.
    static int GetClosedPort(int family = AF_INET)
    {
        LoopbackTcpServer tmp(family);
        return tmp.port();
    }

private:
    int listenFd_;
    int peerFd_;
    int port_;

    static bool waitReadable_(int fd, int timeoutMs)
    {
        return testWaitReadable(fd, timeoutMs);
    }
};

#endif // LOOPBACK_TCP_SERVER_H
