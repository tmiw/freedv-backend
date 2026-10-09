#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
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

// Lets tests observe and manipulate the handler's socket and TLS state (see
// TcpConnectionHandler.h).
class TcpConnectionHandlerTest
{
public:
    // The client's socket, so a test can manipulate it behind the handler's back.
    static int clientSocket(TcpConnectionHandler& conn)
    {
        return (int)conn.socket_.load(std::memory_order_relaxed);
    }

#if defined(ENABLE_TLS_SUPPORT)
    // True if the client's last TLS operation is waiting to write.
    static bool sslWantsWrite(TcpConnectionHandler& conn)
    {
        std::unique_lock<std::mutex> lk(conn.sslMutex_);
        SSL* ssl = conn.ssl_.load(std::memory_order_relaxed);
        return ssl != nullptr && SSL_want_write(ssl);
    }

    // From now on, every TLS write the client attempts reports "try again
    // later" (as on a full socket buffer), however much room the OS has.
    static bool blockSslWrites(TcpConnectionHandler& conn)
    {
        std::unique_lock<std::mutex> lk(conn.sslMutex_);
        SSL* ssl = conn.ssl_.load(std::memory_order_relaxed);
        if (ssl == nullptr)
        {
            return false;
        }

        // The socket BIO (read and write share it). A callback that fails a
        // write before it happens leaves the BIO's ownership untouched.
        BIO_set_callback_ex(SSL_get_rbio(ssl), [](BIO* b, int oper, const char*, size_t, int, long, int ret, size_t*) -> long {
            if (oper == BIO_CB_WRITE)
            {
                BIO_clear_retry_flags(b);
                BIO_set_retry_write(b);
                return -1;
            }
            return ret;
        });
        return true;
    }
#endif // defined(ENABLE_TLS_SUPPORT)
};

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

// Enough data that a send to a peer that isn't reading can't complete,
// whatever the OS buffers on loopback (Windows takes well over 8 MB).
#if defined(_WIN32)
constexpr size_t STALLED_SEND_BYTES = 64 * 1024 * 1024;
#else
constexpr size_t STALLED_SEND_BYTES = 8 * 1024 * 1024;
#endif // defined(_WIN32)

// Records the handler callbacks so tests can wait on them from the main thread.
class TestConnection : public TcpConnectionHandler
{
public:
    // Makes the receive handler slow (1 ms per call), as if the application
    // were busy, so incoming data backs up.
    std::atomic<bool> slowReceive{false};

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
    bool waitForReceived(size_t length, std::chrono::milliseconds timeout = EVENT_TIMEOUT) { return waitFor([&]() { return received_.size() >= length; }, timeout); }
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
        if (slowReceive.load())
        {
            std::this_thread::sleep_for(1ms);
        }
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

    int listenFd = testOpenSocket(AF_INET, SOCK_STREAM);
    int on = 1;
    testSetSockOpt(listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    bool listening = bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) == 0 && listen(listenFd, 1) == 0;
    if (!listening)
    {
        // Another process grabbed the port in the meantime; not a client bug.
        testCloseSocket(listenFd);
        std::cout << "SKIP (port reused) ";
        return report(true);
    }

    result &= CHECK(conn.waitForConnects(1, 10s));

    conn.disconnect().wait();
    testCloseSocket(listenFd);

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

// Closes the socket with an RST instead of a FIN, so the peer's next send
// or read fails outright rather than being accepted into a buffer or
// reported as an orderly EOF.
void resetConnection(int fd)
{
    struct linger lingerOpt = {1, 0};
    testSetSockOpt(fd, SOL_SOCKET, SO_LINGER, &lingerOpt, sizeof(lingerOpt));
}

// Starts a send that can't complete because the server isn't reading, and
// returns its future. Sends `data` twice: Windows accepts any single send in
// full while the data it has queued is under SO_SNDBUF, so there only the
// second send stalls. (Elsewhere the first does, and the second waits.)
std::future<void> startStalledSend(TestConnection& conn, const std::string& data)
{
    conn.send(data.c_str(), data.size());
    return conn.send(data.c_str(), data.size());
}

bool testPlainSendToResetPeer()
{
    std::cout << "Test 22 (plain send to a reset connection fails and disconnects): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // The server resets the connection while the client is mid-way through
    // a send it can't finish (nobody reads): write() must fail, not hang,
    // and the client must treat it as a disconnect.
    const std::string big = makePattern(STALLED_SEND_BYTES);
    auto sendFuture = startStalledSend(conn, big);
    std::this_thread::sleep_for(300ms);
    result &= CHECK(sendFuture.wait_for(0ms) == std::future_status::timeout);
    resetConnection(server.peerFd());
    server.closePeer();

    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(conn.waitForDisconnects(1));
    result &= CHECK(conn.disconnectCount() == 1);

    return report(result);
}

bool testPlainReadFromResetPeer()
{
    std::cout << "Test 23 (reset of an idle plain connection fails the read and disconnects): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // An RST rather than a FIN: read() fails (ECONNRESET) instead of
    // returning 0, which takes the read-error path rather than EOF.
    resetConnection(server.peerFd());
    server.closePeer();

    result &= CHECK(conn.waitForDisconnects(1));
    result &= CHECK(conn.disconnectCount() == 1);
    result &= CHECK(conn.received().empty());

    return report(result);
}

// Sends 1 MB, far more than the client's 128 KB receive buffer, to a client
// whose receive handler is slow: every byte must still arrive, in order.
bool checkBurstToSlowReceiver(TestConnection& conn, std::function<bool(const std::string&)> const& serverSend)
{
    conn.slowReceive = true;
    const std::string big = makePattern(1024 * 1024);
    bool result = CHECK(serverSend(big));
    result &= CHECK(conn.waitForReceived(big.size(), 30s));
    result &= CHECK(conn.received() == big);
    result &= CHECK(conn.disconnectCount() == 0);
    return result;
}

bool testPlainBurstToSlowReceiver()
{
    std::cout << "Test 25 (1 MB burst to a slow receiver arrives intact, plain TCP): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));
    result &= checkBurstToSlowReceiver(conn, [&](const std::string& data) { return server.sendAll(data); });

    return report(result);
}

bool testConnectWhileConnected()
{
    std::cout << "Test 26 (connect() while connected is ignored; the connection keeps working): ";

    LoopbackTcpServer server;
    LoopbackTcpServer otherServer;
    TestConnection conn;

    bool result = CHECK(server.valid() && otherServer.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // Used to replace the running receive thread (std::terminate()).
    auto fut = conn.connect("127.0.0.1", otherServer.port(), false);
    result &= CHECK(fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(!otherServer.accept(500));
    result &= CHECK(conn.connectCount() == 1);

    const std::string msg = "still connected";
    conn.send(msg.c_str(), msg.size());
    std::string got(msg.size(), '\0');
    result &= CHECK(server.recvExact(&got[0], msg.size(), IO_TIMEOUT_MS) && got == msg);
    result &= CHECK(server.sendAll(msg));
    result &= CHECK(conn.waitForReceived(msg.size()));
    result &= CHECK(conn.received() == msg);
    result &= CHECK(conn.disconnectCount() == 0);

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

        path = testMakeTempFile("fdvcert");
        FILE* fp = path.empty() ? nullptr : fopen(path.c_str(), "w");
        if (fp == nullptr)
        {
            return false;
        }
        PEM_write_X509(fp, cert);
        fclose(fp);
        return true;
    }

    ~TestCertificate()
    {
        if (!path.empty())
        {
            std::remove(path.c_str());
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
    explicit TlsServerSession(TestCertificate& cert, bool tls12Only = false)
        : ctx_(SSL_CTX_new(TLS_server_method()))
        , ssl_(nullptr)
    {
        SSL_CTX_use_certificate(ctx_, cert.cert);
        SSL_CTX_use_PrivateKey(ctx_, cert.key);
        if (tls12Only)
        {
            SSL_CTX_set_max_proto_version(ctx_, TLS1_2_VERSION);
        }
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

    // TLS 1.2 only: sends a HelloRequest, so the client has to start a new
    // handshake from inside SSL_read() (i.e. reading obliges it to write).
    // Returns false if this TLS library won't renegotiate.
    bool startRenegotiation()
    {
        if (SSL_renegotiate(ssl_) != 1)
        {
            return false;
        }
        int rv = SSL_do_handshake(ssl_);
        int err = SSL_get_error(ssl_, rv);
        return rv == 1 || err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE;
    }

    // As startRenegotiation(), but returns as soon as the HelloRequest is
    // sent instead of blocking until the client answers.
    bool startRenegotiationNonBlocking()
    {
        testSetNonBlocking(SSL_get_fd(ssl_));
        return startRenegotiation();
    }

#if defined(SSL_KEY_UPDATE_REQUESTED)
    // Asks the client to update its keys too (TLS 1.3), which it must answer
    // with a KeyUpdate of its own -- i.e. reading obliges it to write.
    bool requestKeyUpdate()
    {
        return SSL_key_update(ssl_, SSL_KEY_UPDATE_REQUESTED) == 1;
    }
#endif // defined(SSL_KEY_UPDATE_REQUESTED)

private:
    SSL_CTX* ctx_;
    SSL* ssl_;
};

void setPeerRecvTimeout(int fd, int seconds)
{
    testSetRecvTimeout(fd, seconds * 1000);
    testSetSendTimeout(fd, seconds * 1000);
}

bool testTlsRoundTrip(TestCertificate& cert)
{
    std::cout << "Test 10 (TLS handshake, verified certificate, data both ways): ";

    testSetEnv("SSL_CERT_FILE", cert.path.c_str());

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
    std::string emptyTrustFile = testMakeTempFile("fdvempty");
    testSetEnv("SSL_CERT_FILE", emptyTrustFile.c_str());

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

    std::remove(emptyTrustFile.c_str());
    testSetEnv("SSL_CERT_FILE", cert.path.c_str());

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

bool testTlsHandshakeTimeout()
{
    std::cout << "Test 14 (stalled TLS handshake times out after ~10 s): ";

    // As in test 13 the server never answers, but nobody calls disconnect():
    // the client must give up on its own after TLS_HANDSHAKE_TIMEOUT (10 s)
    // and close the socket, rather than waiting forever.
    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    auto start = std::chrono::steady_clock::now();
    auto connectFuture = conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));

    result &= CHECK(connectFuture.wait_for(20s) == std::future_status::ready);
    auto elapsed = std::chrono::steady_clock::now() - start;
    result &= CHECK(elapsed >= 9s);
    result &= CHECK(elapsed < 15s);
    result &= CHECK(conn.connectCount() == 0);
    result &= CHECK(server.waitForPeerClose(IO_TIMEOUT_MS));

    return report(result);
}

// Common setup for the tests below: a verified TLS connection to a loopback
// server whose SSL object the test drives directly.
struct TlsFixture
{
    LoopbackTcpServer server;
    TestConnection conn;
    TlsServerSession tls;
    bool ok = false;

    explicit TlsFixture(TestCertificate& cert, bool tls12Only = false)
        : tls(cert, tls12Only)
    {
        testSetEnv("SSL_CERT_FILE", cert.path.c_str());
        conn.connect("localhost", server.port(), false, true);
        ok = server.valid() && server.accept(ACCEPT_TIMEOUT_MS);
        if (ok)
        {
            setPeerRecvTimeout(server.peerFd(), 10);
            ok = tls.handshake(server.peerFd()) && conn.waitForConnects(1);
        }
    }
};

bool testTlsSendToSlowReader(TestCertificate& cert)
{
    std::cout << "Test 15 (TLS send blocks on a full socket buffer, then completes): ";

    // 8 MB is far more than the loopback socket buffers hold, so SSL_write()
    // keeps returning SSL_ERROR_WANT_WRITE until the server starts reading.
    TlsFixture f(cert);
    bool result = CHECK(f.ok);

    const std::string big = makePattern(STALLED_SEND_BYTES);
    auto sendFuture = f.conn.send(big.c_str(), big.size());
    std::this_thread::sleep_for(500ms);
    result &= CHECK(sendFuture.wait_for(0ms) == std::future_status::timeout);

    std::string got;
    result &= CHECK(f.tls.readExact(got, big.size()));
    result &= CHECK(got == big);
    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(f.conn.disconnectCount() == 0);

    return report(result);
}

bool testBlockedSendAbandonedOnDisconnect(TestCertificate& cert)
{
    std::cout << "Test 16 (disconnect abandons a send blocked on a full buffer): ";

    bool result = true;

    // Plain TCP and TLS: the peer never reads, so the send can't finish; a
    // disconnect must give up on it promptly instead of waiting forever.
    for (bool useTls : {false, true})
    {
        std::unique_ptr<TlsFixture> tlsFixture;
        LoopbackTcpServer plainServer;
        TestConnection plainConn;
        TestConnection* conn = &plainConn;
        if (useTls)
        {
            tlsFixture = std::make_unique<TlsFixture>(cert);
            result &= CHECK(tlsFixture->ok);
            conn = &tlsFixture->conn;
        }
        else
        {
            plainConn.connect("127.0.0.1", plainServer.port(), false);
            result &= CHECK(plainServer.accept(ACCEPT_TIMEOUT_MS));
            result &= CHECK(plainConn.waitForConnects(1));
        }

        const std::string big = makePattern(STALLED_SEND_BYTES);
        auto sendFuture = startStalledSend(*conn, big);
        std::this_thread::sleep_for(300ms);
        result &= CHECK(sendFuture.wait_for(0ms) == std::future_status::timeout);

        auto start = std::chrono::steady_clock::now();
        auto disconnectFuture = conn->disconnect();
        result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
        result &= CHECK(disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
        result &= CHECK(std::chrono::steady_clock::now() - start < 3s);
    }

    return report(result);
}

bool testTlsSendToResetPeer(TestCertificate& cert)
{
    std::cout << "Test 17 (TLS send to a reset connection fails and disconnects): ";

    TlsFixture f(cert);
    bool result = CHECK(f.ok);

    // The server resets the connection while the client is mid-way through a
    // large send: SSL_write() must fail (not hang) and the client must treat
    // it as a disconnect.
    const std::string big = makePattern(STALLED_SEND_BYTES);
    auto sendFuture = f.conn.send(big.c_str(), big.size());
    std::this_thread::sleep_for(300ms);
    resetConnection(f.server.peerFd());
    f.server.closePeer();

    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(f.conn.waitForDisconnects(1));

    return report(result);
}

bool testTlsCorruptRecord(TestCertificate& cert)
{
    std::cout << "Test 18 (corrupted TLS record from the server disconnects): ";

    TlsFixture f(cert);
    bool result = CHECK(f.ok);

    // A well-formed TLS 1.2+ application-data record header followed by bytes
    // that won't decrypt, written straight to the socket behind SSL's back.
    std::string bogus = "\x17\x03\x03";
    bogus += (char)0x00;
    bogus += (char)0x40;
    bogus += std::string(0x40, '\xAA');
    result &= CHECK(f.server.sendAll(bogus));

    result &= CHECK(f.conn.waitForDisconnects(1));
    result &= CHECK(f.conn.received().empty());

    return report(result);
}

#if defined(SSL_KEY_UPDATE_REQUESTED)
bool testTlsKeyUpdateWhileSendBlocked(TestCertificate& cert)
{
    std::cout << "Test 19 (TLS 1.3 key update while the client's send is blocked): ";

    TlsFixture f(cert);
    bool result = CHECK(f.ok);

    // Request a key update while the client's send is stalled on a full
    // buffer. (OpenSSL queues the client's KeyUpdate response for its next
    // write rather than sending it from SSL_read().) Once the server drains
    // the connection, everything must complete with data intact and the
    // connection must keep working under the new keys.
    const std::string big = makePattern(STALLED_SEND_BYTES);
    auto sendFuture = f.conn.send(big.c_str(), big.size());
    std::this_thread::sleep_for(300ms);

    result &= CHECK(f.tls.requestKeyUpdate());
    const std::string msg = "after key update";
    result &= CHECK(f.tls.write(msg));
    std::this_thread::sleep_for(300ms);

    std::string got;
    result &= CHECK(f.tls.readExact(got, big.size()));
    result &= CHECK(got == big);
    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(f.conn.waitForReceived(msg.size()));
    result &= CHECK(f.conn.received() == msg);

    // The connection must still work in both directions after the update.
    const std::string after = "still alive";
    f.conn.send(after.c_str(), after.size());
    result &= CHECK(f.tls.readExact(got, after.size()) && got == after);

    return report(result);
}

#endif // defined(SSL_KEY_UPDATE_REQUESTED)

// TLS 1.2 renegotiation while the client's send is stalled on a full buffer:
// the stalled SSL_write() picks up the new handshake and has to wait for the
// server's half of it (SSL_write() -> SSL_ERROR_WANT_READ) before it can
// continue sending application data.
bool testTlsRenegotiationWhileSendBlocked(TestCertificate& cert)
{
    std::cout << "Test 20 (TLS 1.2 renegotiation while the client's send is blocked): ";

    TlsFixture f(cert, true);
    bool result = CHECK(f.ok);

    const std::string big = makePattern(STALLED_SEND_BYTES);
    auto sendFuture = f.conn.send(big.c_str(), big.size());
    std::this_thread::sleep_for(300ms);

    if (!f.tls.startRenegotiation())
    {
        std::cout << "SKIP (TLS library won't renegotiate) ";
        std::string drain;
        f.tls.readExact(drain, big.size());
        return report(result);
    }
    std::this_thread::sleep_for(300ms);

    // Drain the client's data (SSL_read() on the server also completes the
    // renegotiation handshake), then check it arrived intact.
    std::string got;
    result &= CHECK(f.tls.readExact(got, big.size()));
    result &= CHECK(got == big);
    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);

    // And the renegotiated connection works in both directions.
    const std::string msg = "after renegotiation";
    result &= CHECK(f.tls.write(msg));
    result &= CHECK(f.conn.waitForReceived(msg.size()));
    result &= CHECK(f.conn.received() == msg);
    f.conn.send(msg.c_str(), msg.size());
    result &= CHECK(f.tls.readExact(got, msg.size()) && got == msg);
    result &= CHECK(f.conn.disconnectCount() == 0);

    return report(result);
}

// TLS 1.2 renegotiation while the client can't write: its SSL_read() has to
// send a ClientHello and can't (SSL_read() -> SSL_ERROR_WANT_WRITE), and the
// socket isn't writable either. A disconnect must still complete promptly
// rather than waiting for the socket to drain.
bool testTlsReadWantsWriteThenDisconnect(TestCertificate& cert)
{
    std::cout << "Test 21 (disconnect while SSL_read() waits to write): ";

    TlsFixture f(cert, true);
    bool result = CHECK(f.ok);
    int clientFd = TcpConnectionHandlerTest::clientSocket(f.conn);
    result &= CHECK(clientFd >= 0);

    // Make the client's TLS writes fail as if its socket were full, then ask
    // it to renegotiate: SSL_read() has to send a ClientHello and can't.
    result &= CHECK(TcpConnectionHandlerTest::blockSslWrites(f.conn));
    if (!f.tls.startRenegotiationNonBlocking())
    {
        std::cout << "SKIP (TLS library won't renegotiate) ";
        return report(result);
    }

    auto waitStart = std::chrono::steady_clock::now();
    while (!TcpConnectionHandlerTest::sslWantsWrite(f.conn) && f.conn.disconnectCount() == 0 &&
           std::chrono::steady_clock::now() - waitStart < EVENT_TIMEOUT)
    {
        std::this_thread::sleep_for(10ms);
    }
    if (f.conn.disconnectCount() > 0)
    {
        std::cout << "SKIP (client refused to renegotiate) ";
        return report(result);
    }
    result &= CHECK(TcpConnectionHandlerTest::sslWantsWrite(f.conn));

    // Now really fill the client's socket buffer (with raw bytes the server
    // never reads, so the stream's integrity doesn't matter), so its receive
    // thread waits for the socket to become writable. Data keeps draining
    // into the server's receive buffer for a while, which the OS may also
    // grow, so keep topping up until nothing more fits for a few rounds.
    if (clientFd >= 0)
    {
        char junk[4096];
        memset(junk, 0, sizeof(junk));
        for (int quietRounds = 0; quietRounds < 3;)
        {
            bool wroteAny = false;
            for (int chunk : {(int)sizeof(junk), 1})
            {
                // The handler's socket is non-blocking, so this stops when full.
                while (::send(clientFd, junk, chunk, 0) > 0)
                {
                    wroteAny = true;
                }
            }
            quietRounds = wroteAny ? 0 : quietRounds + 1;
            std::this_thread::sleep_for(100ms);
        }
    }

    auto start = std::chrono::steady_clock::now();
    auto disconnectFuture = f.conn.disconnect();
    bool finished = disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready;
    result &= CHECK(finished);
    result &= CHECK(std::chrono::steady_clock::now() - start < 3s);
    if (!finished)
    {
        // The handler can't be destroyed while its receive thread is stuck.
        std::cout << "FAIL (disconnect hung)" << std::endl;
        _exit(1);
    }

    return report(result);
}

// TLS 1.2 renegotiation the server never completes: the client sends its
// ClientHello and waits for the server's reply, so its next SSL_write() has
// to wait to read (SSL_write() -> SSL_ERROR_WANT_READ). A disconnect must
// abandon that send promptly.
bool testTlsSendWaitingToReadAbandonedOnDisconnect(TestCertificate& cert)
{
    std::cout << "Test 24 (disconnect abandons a send waiting on a stalled renegotiation): ";

    TlsFixture f(cert, true);
    bool result = CHECK(f.ok);

    if (!f.tls.startRenegotiationNonBlocking())
    {
        std::cout << "SKIP (TLS library won't renegotiate) ";
        return report(result);
    }
    std::this_thread::sleep_for(300ms);
    if (f.conn.disconnectCount() > 0)
    {
        std::cout << "SKIP (client refused to renegotiate) ";
        return report(result);
    }

    const std::string msg = "stuck behind the handshake";
    auto sendFuture = f.conn.send(msg.c_str(), msg.size());
    std::this_thread::sleep_for(300ms);
    result &= CHECK(sendFuture.wait_for(0ms) == std::future_status::timeout);

    auto start = std::chrono::steady_clock::now();
    auto disconnectFuture = f.conn.disconnect();
    bool finished = sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready &&
                    disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready;
    result &= CHECK(finished);
    result &= CHECK(std::chrono::steady_clock::now() - start < 3s);
    if (!finished)
    {
        // The handler can't be destroyed while its send is stuck.
        std::cout << "FAIL (send never abandoned)" << std::endl;
        _exit(1);
    }

    return report(result);
}

bool testTlsBurstToSlowReceiver(TestCertificate& cert)
{
    std::cout << "Test 25b (1 MB burst to a slow receiver arrives intact, TLS): ";

    TlsFixture f(cert);
    bool result = CHECK(f.ok);
    result &= checkBurstToSlowReceiver(f.conn, [&](const std::string& data) { return f.tls.write(data); });

    return report(result);
}

#endif // defined(ENABLE_TLS_SUPPORT)

} // namespace

int main(int, char**)
{
    // The fake servers here write to clients that may already have hung up
    // (e.g. after rejecting a TLS certificate). Report that as a failed write
    // rather than letting SIGPIPE kill the test.
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif // !defined(_WIN32)

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
    result &= testTlsHandshakeTimeout();
    result &= testTlsSendToSlowReader(cert);
    result &= testBlockedSendAbandonedOnDisconnect(cert);
    result &= testTlsSendToResetPeer(cert);
    result &= testTlsCorruptRecord(cert);
#if defined(SSL_KEY_UPDATE_REQUESTED)
    result &= testTlsKeyUpdateWhileSendBlocked(cert);
#endif // defined(SSL_KEY_UPDATE_REQUESTED)
    result &= testTlsRenegotiationWhileSendBlocked(cert);
    result &= testTlsReadWantsWriteThenDisconnect(cert);
    result &= testTlsSendWaitingToReadAbandonedOnDisconnect(cert);
    result &= testTlsBurstToSlowReceiver(cert);
#endif // defined(ENABLE_TLS_SUPPORT)

    // Numbered after the TLS tests, which were written first.
    result &= testPlainSendToResetPeer();
    result &= testPlainReadFromResetPeer();
    result &= testPlainBurstToSlowReceiver();
    result &= testConnectWhileConnected();

    return result ? 0 : -1;
}
