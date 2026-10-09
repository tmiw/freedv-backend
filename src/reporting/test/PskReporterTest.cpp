#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>

#include "../pskreporter.h"

// Grants access to PskReporter's private encoding helpers so the wire format
// can be verified without depending on network reachability of the real
// PSK Reporter server. Kept at namespace scope to match the friend
// declaration in pskreporter.h.
class PskReporterTest
{
public:
    static int rxDataSize(PskReporter& reporter) { return reporter.getRxDataSize_(); }
    static int txDataSize(PskReporter& reporter) { return reporter.getTxDataSize_(); }
    static void encodeRx(PskReporter& reporter, char* buf) { reporter.encodeReceiverRecord_(buf); }
    static void encodeTx(PskReporter& reporter, char* buf) { reporter.encodeSenderRecords_(buf); }
    static bool reportCommon(PskReporter& reporter) { return reporter.reportCommon_(); }
    static void clearRecords(PskReporter& reporter) { reporter.recordList_.clear(); }
    static size_t recordCount(PskReporter& reporter) { return reporter.recordList_.size(); }
};

namespace {

uint64_t readBigEndian(const char* p, int bytes)
{
    uint64_t value = 0;
    for (int i = 0; i < bytes; i++)
    {
        value = (value << 8) | static_cast<unsigned char>(p[i]);
    }
    return value;
}

bool testSenderRecordEncoding()
{
    std::cout << "Test 1 (SenderRecord size and wire encoding): ";

    uint64_t freqHz = 14236000;
    SenderRecord record("N1DQ", freqHz, -5);
    bool result = (record.recordSize() == 23);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    record.encode(buf);

    // callsign: length-prefixed
    result &= (static_cast<unsigned char>(buf[0]) == 4);
    result &= (memcmp(buf + 1, "N1DQ", 4) == 0);

    // frequency: 5-byte big-endian
    result &= (readBigEndian(buf + 5, 5) == freqHz);

    // SNR: one signed byte
    result &= (buf[10] == static_cast<char>(-5));

    // mode: length-prefixed "FREEDV"
    result &= (static_cast<unsigned char>(buf[11]) == 6);
    result &= (memcmp(buf + 12, "FREEDV", 6) == 0);

    // info source
    result &= (static_cast<unsigned char>(buf[18]) == 1);

    // flow start time: 4-byte big-endian, close to now
    uint32_t flowTime = static_cast<uint32_t>(readBigEndian(buf + 19, 4));
    uint64_t now = static_cast<uint64_t>(time(nullptr));
    result &= (flowTime <= now + 2) && (flowTime + 10 >= now);

    // Frequencies above 32 bits must use all 5 frequency bytes.
    SenderRecord big("W1AW", 10000000000ULL, 0);
    memset(buf, 0, sizeof(buf));
    big.encode(buf);
    result &= (readBigEndian(buf + 5, 5) == 10000000000ULL);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testRxDataSizePadding()
{
    std::cout << "Test 2 (RX record data size pads to 4-byte boundary): ";

    // 4 + (1+4) + (1+6) + (1+10) = 27 -> padded to 28.
    PskReporter aligned("K6AQ", "DM12kw", "FreeDV 1.3");
    bool result = (PskReporterTest::rxDataSize(aligned) == 28);

    // 4 + (1+4) + (1+6) + (1+6) = 23 -> padded to 24.
    PskReporter padded("K6AQ", "DM12kw", "FreeDV");
    result &= (PskReporterTest::rxDataSize(padded) == 24);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testReceiverRecordEncoding()
{
    std::cout << "Test 3 (receiver record header and fields): ";

    PskReporter reporter("K6AQ", "DM12kw", "FreeDV 1.3");
    int size = PskReporterTest::rxDataSize(reporter);
    bool result = (size == 28);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    PskReporterTest::encodeRx(reporter, buf);

    result &= (static_cast<unsigned char>(buf[0]) == 0x99);
    result &= (static_cast<unsigned char>(buf[1]) == 0x92);
    result &= (readBigEndian(buf + 2, 2) == static_cast<uint64_t>(size));

    result &= (static_cast<unsigned char>(buf[4]) == 4);
    result &= (memcmp(buf + 5, "K6AQ", 4) == 0);
    result &= (static_cast<unsigned char>(buf[9]) == 6);
    result &= (memcmp(buf + 10, "DM12kw", 6) == 0);
    result &= (static_cast<unsigned char>(buf[16]) == 10);
    result &= (memcmp(buf + 17, "FreeDV 1.3", 10) == 0);

    // Padding bytes up to the aligned size must be zero.
    result &= (buf[27] == 0);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testSenderRecordsEncoding()
{
    std::cout << "Test 4 (TX record list size and encoding): ";

    PskReporter reporter("K6AQ", "DM12kw", "FreeDV 1.3");
    bool result = (PskReporterTest::txDataSize(reporter) == 0);

    uint64_t freqHz = 14236000;
    reporter.addReceiveRecord("N1DQ", "1600X", freqHz, -5);

    // 4 (header+size) + 23 (one record) = 27 -> padded to 28.
    int size = PskReporterTest::txDataSize(reporter);
    result &= (size == 28);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    PskReporterTest::encodeTx(reporter, buf);

    result &= (static_cast<unsigned char>(buf[0]) == 0x99);
    result &= (static_cast<unsigned char>(buf[1]) == 0x93);
    result &= (readBigEndian(buf + 2, 2) == static_cast<uint64_t>(size));

    // The single sender record follows the SenderRecord layout from Test 1.
    result &= (static_cast<unsigned char>(buf[4]) == 4);
    result &= (memcmp(buf + 5, "N1DQ", 4) == 0);
    result &= (readBigEndian(buf + 9, 5) == freqHz);
    result &= (buf[14] == static_cast<char>(-5));
    result &= (static_cast<unsigned char>(buf[15]) == 6);
    result &= (memcmp(buf + 16, "FREEDV", 6) == 0);
    result &= (static_cast<unsigned char>(buf[22]) == 1);

    // Clear before destruction so ~PskReporter doesn't fire a duplicate
    // network report.
    PskReporterTest::clearRecords(reporter);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testReportCommonClearsPendingRecords()
{
    std::cout << "Test 5 (reportCommon_ drains the pending record list): ";

    PskReporter reporter("TEST1", "DM12kw", "FreeDV Test");
    reporter.addReceiveRecord("TEST2", "1600X", 14236000, 5);
    reporter.addReceiveRecord("TEST3", "1600X", 14236000, 6);
    bool result = (PskReporterTest::recordCount(reporter) == 2);

    // Return value depends on network reachability of the real PSK Reporter
    // server, so only the record-list drain is asserted here.
    PskReporterTest::reportCommon(reporter);
    result &= (PskReporterTest::recordCount(reporter) == 0);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testAutoFlushThresholdBoundary()
{
    std::cout << "Test 6 (49 records do not trigger an early flush): ";

    PskReporter reporter("TEST1", "DM12kw", "FreeDV Test");
    bool result = true;
    for (int i = 0; i < 49; i++)
    {
        reporter.addReceiveRecord("TEST2", "1600X", 14236000, static_cast<signed char>(i % 50));
    }
    result &= (PskReporterTest::recordCount(reporter) == 49);

    // Clear before destruction to avoid a network send from the destructor.
    PskReporterTest::clearRecords(reporter);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    bool result = true;

    result &= testSenderRecordEncoding();
    result &= testRxDataSizePadding();
    result &= testReceiverRecordEncoding();
    result &= testSenderRecordsEncoding();
    result &= testReportCommonClearsPendingRecords();
    result &= testAutoFlushThresholdBoundary();

    return result ? 0 : -1;
}
