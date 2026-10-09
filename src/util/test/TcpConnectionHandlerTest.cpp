#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "../TcpConnectionHandler.h"
#include "LoopbackTcpServer.h"

#if defined(ENABLE_TLS_SUPPORT)
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif // defined(ENABLE_TLS_SUPPORT)

using namespace std::chrono_literals;

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


constexpr int ACCEPT_TIMEOUT_MS = 5000;
constexpr int IO_TIMEOUT_MS = 5000;
constexpr auto EVENT_TIMEOUT = 5s;

// Records the handler callbacks so tests can wait on them from the main thread.
class TestConnection : public TcpConnectionHandler
{
public:
    virtual ~TestConnection()
    {
        // TcpConnectionHandler's destructor would call our onDisconnect_()
        // after this subclass is gone, so disconnect while we still exist.
        enableReconnect_.store(false, std::memory_order_relaxed);
        disconnect().wait();
        waitForAllTasksComplete_();
    }

    int connectCount()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return connectCount_;
    }

    int disconnectCount()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return disconnectCount_;
    }

    std::string received()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return received_;
    }

    void markRecvEnd()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        recvEndCount_++;
        cv_.notify_all();
    }

    bool waitFor(std::function<bool()> const& pred, std::chrono::milliseconds timeout = EVENT_TIMEOUT)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return cv_.wait_for(lk, timeout, pred);
    }

    bool waitForConnects(int count, std::chrono::milliseconds timeout = EVENT_TIMEOUT) { return waitFor([&]() { return connectCount_ >= count; }, timeout); }
    bool waitForDisconnects(int count) { return waitFor([&]() { return disconnectCount_ >= count; }); }
    bool waitForReceived(size_t length) { return waitFor([&]() { return received_.size() >= length; }); }
    bool waitForRecvEnd() { return waitFor([&]() { return recvEndCount_ > 0; }); }

protected:
    virtual void onConnect_() override
    {
        std::unique_lock<std::mutex> lk(mutex_);
        connectCount_++;
        cv_.notify_all();
    }

    virtual void onDisconnect_() override
    {
        std::unique_lock<std::mutex> lk(mutex_);
        disconnectCount_++;
        cv_.notify_all();
    }

    virtual void onReceive_(char* buf, int length) override
    {
        std::unique_lock<std::mutex> lk(mutex_);
        received_.append(buf, length);
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int connectCount_ = 0;
    int disconnectCount_ = 0;
    int recvEndCount_ = 0;
    std::string received_;
};

std::string makePattern(size_t length)
{
    std::string result(length, '\0');
    for (size_t i = 0; i < length; i++)
    {
        result[i] = (char)('A' + (i * 7 + i / 251) % 26);
    }
    return result;
}

bool report(bool result)
{
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testConnectSendReceive()
{
    std::cout << "Test 1 (connect, send and receive over loopback): ";

    LoopbackTcpServer server;
    TestConnection conn;
    conn.setOnRecvEndFn([&]() { conn.markRecvEnd(); });

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // Client -> server.
    const std::string clientMsg = "hello from client";
    conn.send(clientMsg.c_str(), clientMsg.size());
    std::string gotClientMsg(clientMsg.size(), '\0');
    result &= CHECK(server.recvExact(&gotClientMsg[0], gotClientMsg.size(), IO_TIMEOUT_MS));
    result &= CHECK((gotClientMsg == clientMsg));

    // Server -> client, followed by the end-of-receive callback.
    const std::string serverMsg = "hello from server";
    result &= CHECK(server.sendAll(serverMsg));
    result &= CHECK(conn.waitForReceived(serverMsg.size()));
    result &= CHECK((conn.received() == serverMsg));
    result &= CHECK(conn.waitForRecvEnd());

    return report(result);
}

bool testLargeTransfers()
{
    std::cout << "Test 2 (multi-chunk transfers in both directions): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // 4 MB outbound is larger than the loopback socket buffer, so sendImpl_()
    // has to loop on partial writes while the server drains the data.
    const std::string bigOut = makePattern(4 * 1024 * 1024);
    auto sendFuture = conn.send(bigOut.c_str(), bigOut.size());
    std::string gotOut(bigOut.size(), '\0');
    result &= CHECK(server.recvExact(&gotOut[0], gotOut.size(), IO_TIMEOUT_MS));
    result &= CHECK((sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((gotOut == bigOut));

    // 64 KB inbound spans many 1 KB reads but fits in the 128 KB receive FIFO.
    const std::string bigIn = makePattern(64 * 1024);
    result &= CHECK(server.sendAll(bigIn));
    result &= CHECK(conn.waitForReceived(bigIn.size()));
    result &= CHECK((conn.received() == bigIn));

    return report(result);
}

bool testServerCloseTriggersDisconnect()
{
    std::cout << "Test 3 (server closing the socket fires onDisconnect_): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    server.closePeer();
    result &= CHECK(conn.waitForDisconnects(1));

    // Without reconnect enabled, nothing should try to connect again.
    result &= CHECK(!server.accept(500));
    result &= CHECK((conn.connectCount() == 1));

    // Sending after the disconnect is a silent no-op.
    conn.send("x", 1).wait();

    return report(result);
}

bool testClientDisconnect()
{
    std::cout << "Test 4 (client disconnect closes socket exactly once): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());

    // Disconnecting while not connected must not fire the handler.
    conn.disconnect().wait();
    result &= CHECK((conn.disconnectCount() == 0));

    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    conn.disconnect().wait();
    result &= CHECK((conn.disconnectCount() == 1));
    result &= CHECK(server.waitForPeerClose(IO_TIMEOUT_MS));

    // A second disconnect is a no-op.
    conn.disconnect().wait();
    result &= CHECK((conn.disconnectCount() == 1));

    return report(result);
}

bool testConnectionRefused()
{
    std::cout << "Test 5 (refused connection without reconnect): ";

    TestConnection conn;
    int port = LoopbackTcpServer::GetClosedPort();

    bool result = CHECK((port > 0));
    auto fut = conn.connect("127.0.0.1", port, false);
    result &= CHECK((fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((conn.connectCount() == 0));
    result &= CHECK((conn.disconnectCount() == 0));

    return report(result);
}

bool testReconnectAfterServerClose()
{
    std::cout << "Test 6 (reconnect timer re-establishes a dropped connection): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // Drop the connection; the client should come back after the reconnect
    // interval (5 s).
    server.closePeer();
    result &= CHECK(conn.waitForDisconnects(1));
    result &= CHECK(server.accept(10000));
    result &= CHECK(conn.waitForConnects(2));

    return report(result);
}

bool testReconnectAfterRefusal()
{
    std::cout << "Test 7 (failed connect with reconnect enabled retries): ";

    // Connect to a port nobody is listening on yet, with reconnect enabled.
    // Then start listening on that port and wait for the retry to land.
    int port = LoopbackTcpServer::GetClosedPort();
    TestConnection conn;
    auto fut = conn.connect("127.0.0.1", port, true);

    bool result = CHECK((fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((conn.connectCount() == 0));

    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    bool listening = bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) == 0 && listen(listenFd, 1) == 0;
    if (!listening)
    {
        // Another process grabbed the port in the meantime; not a client bug.
        close(listenFd);
        std::cout << "SKIP (port reused) ";
        return report(true);
    }

    result &= CHECK(conn.waitForConnects(1, 10s));

    conn.disconnect().wait();
    close(listenFd);

    return report(result);
}

bool testHostnameResolution()
{
    std::cout << "Test 8 (\"localhost\" resolves and falls back to IPv4): ";

    // The server only listens on 127.0.0.1, so if "localhost" also resolves
    // to ::1 that attempt is refused and the IPv4 address must win.
    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("localhost", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    return report(result);
}

bool testUnresolvableHost()
{
    std::cout << "Test 9 (unresolvable hostname fails cleanly): ";

    // RFC 6761 reserves .invalid, so this can never resolve.
    TestConnection conn;
    auto fut = conn.connect("freedv-backend-test.invalid", 80, false);

    bool result = CHECK(fut.wait_for(30s) == std::future_status::ready);
    result &= CHECK((conn.connectCount() == 0));

    return report(result);
}

#if defined(ENABLE_TLS_SUPPORT)

// Self-signed certificate for "localhost", generated at runtime so the test
// carries no key material. The client trusts it via SSL_CERT_FILE.
struct TestCertificate
{
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    std::string path;

    bool generate()
    {
        EVP_PKEY_CTX* keyCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        if (keyCtx == nullptr ||
            EVP_PKEY_keygen_init(keyCtx) <= 0 ||
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(keyCtx, NID_X9_62_prime256v1) <= 0 ||
            EVP_PKEY_keygen(keyCtx, &key) <= 0)
        {
            EVP_PKEY_CTX_free(keyCtx);
            return false;
        }
        EVP_PKEY_CTX_free(keyCtx);

        cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);

        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)"localhost", -1, -1, 0);
        X509_set_issuer_name(cert, name);

        X509V3_CTX extCtx;
        X509V3_set_ctx_nodb(&extCtx);
        X509V3_set_ctx(&extCtx, cert, cert, nullptr, nullptr, 0);
        const char* exts[][2] = {
            {"basicConstraints", "critical,CA:TRUE"},
            {"subjectAltName", "DNS:localhost"},
        };
        for (auto& ext : exts)
        {
            X509_EXTENSION* extension = X509V3_EXT_conf(nullptr, &extCtx, (char*)ext[0], (char*)ext[1]);
            if (extension == nullptr)
            {
                return false;
            }
            X509_add_ext(cert, extension, -1);
            X509_EXTENSION_free(extension);
        }

        if (X509_sign(cert, key, EVP_sha256()) <= 0)
        {
            return false;
        }

        char tmpl[] = "/tmp/fdv_tcp_test_cert_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0)
        {
            return false;
        }
        FILE* fp = fdopen(fd, "w");
        PEM_write_X509(fp, cert);
        fclose(fp);
        path = tmpl;
        return true;
    }

    ~TestCertificate()
    {
        if (!path.empty())
        {
            unlink(path.c_str());
        }
        X509_free(cert);
        EVP_PKEY_free(key);
    }
};

// Runs the server half of a TLS session on the accepted peer socket:
// handshake, read the client's message, reply with an echo.
class TlsServerSession
{
public:
    explicit TlsServerSession(TestCertificate& cert)
        : ctx_(SSL_CTX_new(TLS_server_method()))
        , ssl_(nullptr)
    {
        SSL_CTX_use_certificate(ctx_, cert.cert);
        SSL_CTX_use_PrivateKey(ctx_, cert.key);
    }

    ~TlsServerSession()
    {
        if (ssl_ != nullptr)
        {
            SSL_free(ssl_);
        }
        SSL_CTX_free(ctx_);
    }

    bool handshake(int fd)
    {
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, fd);
        return SSL_accept(ssl_) == 1;
    }

    bool readExact(std::string& out, size_t length)
    {
        out.resize(length);
        size_t offset = 0;
        while (offset < length)
        {
            int numRead = SSL_read(ssl_, &out[offset], length - offset);
            if (numRead <= 0)
            {
                return false;
            }
            offset += numRead;
        }
        return true;
    }

    bool write(const std::string& data)
    {
        return SSL_write(ssl_, data.data(), data.size()) == (int)data.size();
    }

    // Waits for the client's close_notify (or EOF).
    bool waitForShutdown()
    {
        char buf[64];
        return SSL_read(ssl_, buf, sizeof(buf)) <= 0;
    }

private:
    SSL_CTX* ctx_;
    SSL* ssl_;
};

void setPeerRecvTimeout(int fd, int seconds)
{
    struct timeval tv = {seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool testTlsRoundTrip(TestCertificate& cert)
{
    std::cout << "Test 10 (TLS handshake, verified certificate, data both ways): ";

    setenv("SSL_CERT_FILE", cert.path.c_str(), 1);

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    setPeerRecvTimeout(server.peerFd(), 5);

    TlsServerSession tls(cert);
    result &= CHECK(tls.handshake(server.peerFd()));
    result &= CHECK(conn.waitForConnects(1));

    const std::string clientMsg = "hello over tls";
    conn.send(clientMsg.c_str(), clientMsg.size());
    std::string gotClientMsg;
    result &= CHECK(tls.readExact(gotClientMsg, clientMsg.size()));
    result &= CHECK((gotClientMsg == clientMsg));

    // Several SSL_read() calls on the client. An exact multiple of the 1 KB
    // read size also checks that data is dispatched when the last read hits
    // SSL_ERROR_WANT_READ rather than being held until more data arrives.
    const std::string serverMsg = makePattern(16 * 1024);
    result &= CHECK(tls.write(serverMsg));
    result &= CHECK(conn.waitForReceived(serverMsg.size()));
    result &= CHECK((conn.received() == serverMsg));

    conn.disconnect().wait();
    result &= CHECK((conn.disconnectCount() == 1));
    result &= CHECK(tls.waitForShutdown());

    return report(result);
}

bool testTlsUntrustedCertificate(TestCertificate& cert)
{
    std::cout << "Test 11 (TLS with untrusted certificate is rejected): ";

    // Point the client at an empty trust file so our self-signed cert fails
    // validation.
    char tmpl[] = "/tmp/fdv_tcp_test_empty_XXXXXX";
    int fd = mkstemp(tmpl);
    close(fd);
    setenv("SSL_CERT_FILE", tmpl, 1);

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    auto fut = conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    setPeerRecvTimeout(server.peerFd(), 5);

    TlsServerSession tls(cert);
    result &= CHECK(!tls.handshake(server.peerFd()));
    result &= CHECK((fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((conn.connectCount() == 0));

    unlink(tmpl);
    setenv("SSL_CERT_FILE", cert.path.c_str(), 1);

    return report(result);
}

bool testTlsGarbageFromServer()
{
    std::cout << "Test 12 (non-TLS reply during handshake is rejected): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    auto fut = conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(server.sendAll(std::string("HTTP/1.1 400 Bad Request\r\n\r\n")));
    result &= CHECK((fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((conn.connectCount() == 0));

    return report(result);
}

bool testTlsHandshakeCancelled()
{
    std::cout << "Test 13 (disconnect cancels a stalled TLS handshake): ";

    // The server accepts but never answers the ClientHello, so the client sits
    // in its handshake loop until we ask it to disconnect.
    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    auto connectFuture = conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    std::this_thread::sleep_for(300ms);
    result &= CHECK((connectFuture.wait_for(0ms) == std::future_status::timeout));

    auto start = std::chrono::steady_clock::now();
    auto disconnectFuture = conn.disconnect();
    result &= CHECK((connectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((std::chrono::steady_clock::now() - start < 5s));
    result &= CHECK((conn.connectCount() == 0));
    result &= CHECK(server.waitForPeerClose(IO_TIMEOUT_MS));

    return report(result);
}

#endif // defined(ENABLE_TLS_SUPPORT)

} // namespace

int main(int, char**)
{
    bool result = true;

    result &= testConnectSendReceive();
    result &= testLargeTransfers();
    result &= testServerCloseTriggersDisconnect();
    result &= testClientDisconnect();
    result &= testConnectionRefused();
    result &= testReconnectAfterServerClose();
    result &= testReconnectAfterRefusal();
    result &= testHostnameResolution();
    result &= testUnresolvableHost();

#if defined(ENABLE_TLS_SUPPORT)
    TestCertificate cert;
    if (!cert.generate())
    {
        std::cout << "Could not generate test certificate\n";
        return -1;
    }
    result &= testTlsRoundTrip(cert);
    result &= testTlsUntrustedCertificate(cert);
    result &= testTlsGarbageFromServer();
    result &= testTlsHandshakeCancelled();
#endif // defined(ENABLE_TLS_SUPPORT)

    return result ? 0 : -1;
}
