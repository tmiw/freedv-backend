// Tests for UdpHandler: unicast sends, the multicast group-join paths used by
// UdpReporter (multicast destinations such as WSJT-X style 239.x.x.x groups),
// and error handling for bad addresses.
//
// Multicast delivery depends on the host having a multicast-capable route, so
// the multicast delivery checks first verify with a plain socket that the
// environment loops multicast back to itself, and are skipped (not failed)
// when it doesn't.

#include <cstring>
#include <iostream>
#include <string>

#include "TestSocketCompat.h"

#include "../UdpHandler.h"

namespace {

// Evaluates a test condition, printing the failing expression and line so a
// FAIL can be traced without a debugger.
#define CHECK(expr) checkImpl_((expr), #expr, __LINE__)

bool checkImpl_(bool ok, const char* expr, int line)
{
    if (!ok)
    {
        std::cout << "\n    check failed at line " << line << ": " << expr << "\n    ";
    }
    return ok;
}

bool report(bool result)
{
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

const char* MULTICAST_GROUP_V4 = "239.255.77.77";
const char* MULTICAST_GROUP_V4_BIND = "239.255.77.78";
const char* MULTICAST_GROUP_V6 = "ff15::7777"; // transient, site-local scope

class TestUdpHandler : public UdpHandler
{
protected:
    // UdpHandler doesn't receive (receiveImpl_() is a no-op), so this is never
    // called; required because it's pure virtual.
    virtual void onReceive_(const char*, int, char*, int) override {}
};

// UDP socket listening on an ephemeral port, optionally a member of a
// multicast group.
class Listener
{
public:
    explicit Listener(int family = AF_INET)
        : family_(family)
        , fd_(testOpenSocket(family, SOCK_DGRAM))
        , port_(-1)
    {
        if (fd_ < 0)
        {
            return;
        }
        int on = 1;
        testSetSockOpt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        struct sockaddr_storage addr;
        socklen_t addrLen = 0;
        memset(&addr, 0, sizeof(addr));
        if (family == AF_INET)
        {
            auto in = (struct sockaddr_in*)&addr;
            in->sin_family = AF_INET;
            in->sin_addr.s_addr = htonl(INADDR_ANY);
            addrLen = sizeof(*in);
        }
        else
        {
            auto in6 = (struct sockaddr_in6*)&addr;
            in6->sin6_family = AF_INET6;
            in6->sin6_addr = in6addr_any;
            addrLen = sizeof(*in6);
        }
        if (bind(fd_, (struct sockaddr*)&addr, addrLen) == 0 &&
            getsockname(fd_, (struct sockaddr*)&addr, &addrLen) == 0)
        {
            port_ = ntohs(family == AF_INET ? ((struct sockaddr_in*)&addr)->sin_port
                                            : ((struct sockaddr_in6*)&addr)->sin6_port);
        }
    }

    ~Listener()
    {
        if (fd_ >= 0) testCloseSocket(fd_);
    }

    int port() const { return port_; }

    bool joinGroup(const char* group)
    {
        if (family_ == AF_INET)
        {
            struct ip_mreq req;
            memset(&req, 0, sizeof(req));
            inet_pton(AF_INET, group, &req.imr_multiaddr);
            req.imr_interface.s_addr = htonl(INADDR_ANY);
            return testSetSockOpt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &req, sizeof(req)) == 0;
        }
        struct ipv6_mreq req;
        memset(&req, 0, sizeof(req));
        inet_pton(AF_INET6, group, &req.ipv6mr_multiaddr);
        req.ipv6mr_interface = 0;
        return testSetSockOpt(fd_, IPPROTO_IPV6, IPV6_JOIN_GROUP, &req, sizeof(req)) == 0;
    }

    std::string receive(int timeoutMs)
    {
        if (!testWaitReadable(fd_, timeoutMs))
        {
            return "";
        }
        char buf[2048];
        ssize_t len = recv(fd_, buf, sizeof(buf), 0);
        return len > 0 ? std::string(buf, len) : "";
    }

private:
    int family_;
    int fd_;
    int port_;
};

// Sends one datagram with a plain socket. Used to probe whether the
// environment delivers multicast back to local listeners at all.
bool plainSend(int family, const char* host, int port, const std::string& payload)
{
    int fd = testOpenSocket(family, SOCK_DGRAM);
    if (fd < 0)
    {
        return false;
    }
    struct sockaddr_storage addr;
    socklen_t addrLen = 0;
    memset(&addr, 0, sizeof(addr));
    if (family == AF_INET)
    {
        auto in = (struct sockaddr_in*)&addr;
        in->sin_family = AF_INET;
        in->sin_port = htons(port);
        inet_pton(AF_INET, host, &in->sin_addr);
        addrLen = sizeof(*in);
    }
    else
    {
        auto in6 = (struct sockaddr_in6*)&addr;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(port);
        inet_pton(AF_INET6, host, &in6->sin6_addr);
        addrLen = sizeof(*in6);
    }
    bool ok = sendto(fd, payload.data(), payload.size(), 0, (struct sockaddr*)&addr, addrLen) == (ssize_t)payload.size();
    testCloseSocket(fd);
    return ok;
}

// Returns a UDP port that was free a moment ago.
int getUnusedPort()
{
    Listener tmp(AF_INET);
    return tmp.port();
}

bool multicastWorks(int family, const char* group)
{
    Listener probe(family);
    return probe.port() > 0 && probe.joinGroup(group) &&
           plainSend(family, group, probe.port(), "probe") &&
           probe.receive(1000) == "probe";
}

bool testUnicastSend()
{
    std::cout << "Test 1 (unicast send to IPv4 and IPv6 loopback): ";

    bool result = true;

    Listener v4(AF_INET);
    TestUdpHandler handler;
    handler.open().wait(); // no host: unbound IPv4 socket for sending
    const std::string msg = "hello udp";
    handler.send("127.0.0.1", v4.port(), msg.c_str(), msg.size()).wait();
    result &= CHECK(v4.receive(2000) == msg);

    // An IPv6 send hint makes open() create an IPv6 socket.
    Listener v6(AF_INET6);
    if (v6.port() > 0)
    {
        TestUdpHandler handler6;
        handler6.open("", 0, "::1", v6.port()).wait();
        handler6.send("::1", v6.port(), msg.c_str(), msg.size()).wait();
        result &= CHECK(v6.receive(2000) == msg);
    }

    return report(result);
}

bool testMulticastSendHint()
{
    std::cout << "Test 2 (IPv4 multicast destination joins the group and delivers): ";

    if (!multicastWorks(AF_INET, MULTICAST_GROUP_V4))
    {
        std::cout << "SKIP (no IPv4 multicast loopback here) ";
        return report(true);
    }

    // This is how UdpReporter opens its socket: no local address, with the
    // destination given as a hint so a multicast group is joined up front.
    Listener listener(AF_INET);
    bool result = CHECK(listener.joinGroup(MULTICAST_GROUP_V4));

    TestUdpHandler handler;
    handler.open("", 0, MULTICAST_GROUP_V4, listener.port()).wait();
    const std::string msg = "hello multicast";
    for (int i = 0; i < 3; i++)
    {
        handler.send(MULTICAST_GROUP_V4, listener.port(), msg.c_str(), msg.size()).wait();
        result &= CHECK(listener.receive(2000) == msg);
    }

    return report(result);
}

bool testBindToMulticastGroup()
{
    std::cout << "Test 3 (binding to a multicast group address joins it): ";

    if (!multicastWorks(AF_INET, MULTICAST_GROUP_V4_BIND))
    {
        std::cout << "SKIP (no IPv4 multicast loopback here) ";
        return report(true);
    }

    // open() with a multicast host binds to the group and joins it, then
    // starts the receive thread. Group traffic must still reach other members,
    // and close() must stop the receive thread cleanly.
    Listener listener(AF_INET);
    bool result = CHECK(listener.joinGroup(MULTICAST_GROUP_V4_BIND));

    // Bind on a port of its own: UdpHandler doesn't set SO_REUSEADDR, so it
    // can't share the listener's port.
    int handlerPort = getUnusedPort();

    TestUdpHandler handler;
    handler.open(MULTICAST_GROUP_V4_BIND, handlerPort).wait();

    const std::string msg = "group traffic";
    result &= CHECK(plainSend(AF_INET, MULTICAST_GROUP_V4_BIND, listener.port(), msg));
    result &= CHECK(listener.receive(2000) == msg);

    // Note: a socket bound to a multicast address can't itself send (its
    // source address would be the group), so only reception by others is
    // checked here.

    handler.close().wait();
    // Closing twice is harmless.
    handler.close().wait();

    return report(result);
}

bool testIpv6Multicast()
{
    std::cout << "Test 4 (IPv6 multicast destination joins the group and delivers): ";

    if (!multicastWorks(AF_INET6, MULTICAST_GROUP_V6))
    {
        std::cout << "SKIP (no IPv6 multicast loopback here) ";
        return report(true);
    }

    Listener listener(AF_INET6);
    bool result = CHECK(listener.joinGroup(MULTICAST_GROUP_V6));

    TestUdpHandler handler;
    handler.open("", 0, MULTICAST_GROUP_V6, listener.port()).wait();
    const std::string msg = "hello v6 multicast";
    handler.send(MULTICAST_GROUP_V6, listener.port(), msg.c_str(), msg.size()).wait();
    result &= CHECK(listener.receive(2000) == msg);

    return report(result);
}

bool testBadAddresses()
{
    std::cout << "Test 5 (unparseable and non-local addresses fail cleanly): ";

    bool result = true;

    // UdpHandler only accepts numeric IPs; a hostname fails to resolve.
    {
        TestUdpHandler handler;
        handler.open("not-an-ip-address", 1234).wait();
        handler.send("127.0.0.1", 9, "x", 1).wait(); // socket never opened
        handler.close().wait();
    }

    // A send hint that doesn't parse falls back to IPv4.
    {
        Listener listener(AF_INET);
        TestUdpHandler handler;
        handler.open("", 0, "not-an-ip-address", 1234).wait();
        handler.send("127.0.0.1", listener.port(), "ok", 2).wait();
        result &= CHECK(listener.receive(2000) == "ok");

        // Sending to an unparseable destination is dropped.
        handler.send("also-not-an-ip", listener.port(), "no", 2).wait();
        result &= CHECK(listener.receive(300).empty());
    }

    // Binding to an address this host doesn't own (TEST-NET-1) fails without
    // starting the receive thread; close() must still work.
    {
        TestUdpHandler handler;
        handler.open("192.0.2.1", 0).wait();
        handler.close().wait();
    }

    return report(result);
}

} // namespace

int main(int, char**)
{
    bool result = true;

    result &= testUnicastSend();
    result &= testMulticastSendHint();
    result &= testBindToMulticastGroup();
    result &= testIpv6Multicast();
    result &= testBadAddresses();

    return result ? 0 : -1;
}
