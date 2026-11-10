// Tests for src/net: message bits, Huffman, netchan fragmentation, connectionless packets, delta coders.
// Reference values follow docs/spec/net.md.

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "net/delta.h"
#include "net/huffman.h"
#include "net/msg.h"
#include "net/netchan.h"
#include "net/netfields.h"
#include "net/playerstate.h"
#include "net/scramble.h"
#include "net/oob.h"

using namespace iw2::net;

namespace {

int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

std::string hex(const uint8_t* p, int n) {
    std::string s;
    char b[3];
    for (int i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}

void testMsgMixedPacking() {
    uint8_t buf[64] = {};
    Msg m(buf, sizeof buf);
    m.writeBit1();        // starts bit byte 0
    m.writeByte(0xAB);    // byte 1
    m.writeBits(0x5, 3);  // continues bit byte 0: bits 1..3 = 1,0,1
    m.writeShort(0x1234); // bytes 2..3
    m.writeBits(0xF, 4);  // bit byte 0 bits 4..7
    m.writeBit1();        // new bit byte at 4
    m.writeLong(-2);      // bytes 5..8
    CHECK(m.cursize() == 9);
    CHECK(buf[0] == 0xFB);  // 1 | 1<<1 | 0<<2 | 1<<3 | 0xF<<4
    CHECK(buf[1] == 0xAB);
    CHECK(buf[2] == 0x34 && buf[3] == 0x12);
    CHECK(buf[4] == 0x01);
    CHECK(buf[5] == 0xFE && buf[8] == 0xFF);
    CHECK(m.bitLength() == 9 * 8 - 7);

    m.beginReading();
    CHECK(m.readBit() == 1);
    CHECK(m.readByte() == 0xAB);
    CHECK(m.readBits(3) == 5);
    CHECK(m.readShort() == 0x1234);
    CHECK(m.readBits(4) == 0xF);
    CHECK(m.readBit() == 1);
    CHECK(m.readLong() == -2);
    CHECK(!m.overflowed());
    CHECK(m.readByte() == -1);
    CHECK(m.overflowed());
}

void testMsgOverflowAndStrings() {
    uint8_t buf[6] = {};
    Msg m(buf, sizeof buf);
    m.writeByte(1);
    m.writeByte(2);
    m.writeBits(1, 1);  // 4 bytes free: allowed
    CHECK(!m.overflowed());
    m.writeBits(1, 1);  // continues the open bit byte, still checks free space: 3 free -> overflow
    CHECK(m.overflowed());

    uint8_t sb[2048];
    Msg s(sb, sizeof sb);
    s.writeString(std::string("it\x92s"));
    s.writeString(std::string(1024, 'x'));  // too long: empty string
    s.writeData("a%b\nc", 6);
    s.beginReading();
    CHECK(s.readString() == "it's");
    CHECK(s.readString().empty());
    CHECK(s.readStringLine() == "a.b");
    CHECK(s.readString() == "c");

    uint8_t ab[4];
    Msg a(ab, sizeof ab);
    a.writeAngle16(90.0f);
    a.beginReading();
    float ang = a.readAngle16();
    CHECK(ang > 89.99f && ang < 90.01f);
}

void testHuffmanCodes() {
    const Huffman& h = Huffman::protocol();
    // Reference codes and a hash of all 256 codes (joined by '\n'), see docs/spec/net.md section 3.
    CHECK(h.code(0) == "01");
    CHECK(h.code(1) == "11011");
    CHECK(h.code(' ') == "111011");
    CHECK(h.code('a') == "00111100");
    CHECK(h.code(255) == "001001");
    std::string all;
    size_t minLen = 99, maxLen = 0;
    for (int i = 0; i < 256; ++i) {
        std::string c = h.code(i);
        minLen = std::min(minLen, c.size());
        maxLen = std::max(maxLen, c.size());
        all += c;
        if (i != 255) all += '\n';
    }
    uint64_t fnv = 0xcbf29ce484222325ull;
    for (unsigned char c : all) fnv = (fnv ^ c) * 0x100000001b3ull;
    CHECK(fnv == 0x9c0a3b74488c17e5ull);
    CHECK(minLen == 2 && maxLen == 11);
}

void testHuffmanVectors() {
    const Huffman& h = Huffman::protocol();
    uint8_t out[1024], back[2048];
    const std::string a = "getinfo xxx";
    int n = h.compress(reinterpret_cast<const uint8_t*>(a.data()), int(a.size()), out, sizeof out);
    CHECK(hex(out, n) == "4f61591486a0d97b696909");
    int m = h.decompress(out, n, back, sizeof back);
    CHECK(m >= int(a.size()) && std::memcmp(back, a.data(), a.size()) == 0);

    const std::string b = std::string("\xff\xff\xff\xff", 4) + "statusResponse\n";
    n = h.compress(reinterpret_cast<const uint8_t*>(b.data()), int(b.size()), out, sizeof out);
    CHECK(hex(out, n) == "24499279599e2c306f2d585ee12c431e2c0d");

    uint8_t all[256];
    for (int i = 0; i < 256; ++i) all[i] = uint8_t(i);
    n = h.compress(all, 256, out, sizeof out);
    CHECK(hex(out, 16) == "6e243b3402f2575a7a3169735895b0c0");
    CHECK(hex(out + n - 8, 8) == "eb7c51cbb5d11709");
    m = h.decompress(out, n, back, sizeof back);
    CHECK(m == 257);  // one padding symbol decoded at the end
    CHECK(std::memcmp(back, all, 256) == 0);

    // Single zero byte: code "01" -> 0x02, padding decodes to one more symbol.
    uint8_t z = 0;
    n = h.compress(&z, 1, out, sizeof out);
    CHECK(n == 1 && out[0] == 0x02);
}

struct Wire {
    std::vector<std::vector<uint8_t>> packets;
    Netchan::SendFn fn() {
        return [this](const uint8_t* d, int n) { packets.emplace_back(d, d + n); };
    }
};

bool deliver(Netchan& rx, const std::vector<uint8_t>& p, std::vector<uint8_t>& payload) {
    std::vector<uint8_t> buf(kMaxMsgLen + 16);
    std::memcpy(buf.data(), p.data(), p.size());
    Msg m(buf.data(), int(buf.size()));
    m.setCursize(int(p.size()));
    if (!rx.process(m)) return false;
    payload.assign(buf.data() + m.readcount(), buf.data() + m.cursize());
    return true;
}

void testNetchanSmall() {
    Wire w;
    Netchan tx(NetSide::Client, 0x1234, w.fn());
    Netchan rx(NetSide::Server, 0x1234, nullptr);
    const uint8_t data[] = {1, 2, 3};
    tx.transmit(data, 3);
    CHECK(w.packets.size() == 1);
    const auto& p = w.packets[0];
    CHECK(p.size() == 4 + 2 + 3);
    CHECK(p[0] == 1 && p[3] == 0);      // sequence 1
    CHECK(p[4] == 0x34 && p[5] == 0x12);  // qport from client
    CHECK(tx.outgoingSequence() == 2);
    std::vector<uint8_t> got;
    CHECK(deliver(rx, p, got));
    CHECK(got == std::vector<uint8_t>({1, 2, 3}));
    CHECK(!deliver(rx, p, got));  // duplicate dropped

    // Server -> client packets carry no qport.
    Wire w2;
    Netchan stx(NetSide::Server, 7, w2.fn());
    stx.transmit(data, 3);
    CHECK(w2.packets[0].size() == 4 + 3);
}

void testNetchanFragments(int len) {
    Wire w;
    Netchan tx(NetSide::Server, 0, w.fn());
    Netchan rx(NetSide::Client, 0, nullptr);
    std::vector<uint8_t> msg(static_cast<size_t>(len));
    for (int i = 0; i < len; ++i) msg[size_t(i)] = uint8_t(i * 7 + 3);
    tx.transmit(msg.data(), len);
    while (tx.hasPendingFragments()) tx.transmitNextFragment();
    const size_t expected = size_t(len / kFragmentSize + 1);
    CHECK(w.packets.size() == expected);
    for (const auto& p : w.packets) CHECK((p[3] & 0x80) != 0);  // fragment bit
    // First fragment header: start 0 as a 32-bit long, length 1300 as a short.
    const auto& f0 = w.packets[0];
    CHECK(f0[4] == 0 && f0[5] == 0 && f0[6] == 0 && f0[7] == 0);
    CHECK(f0[8] == (kFragmentSize & 0xff) && f0[9] == (kFragmentSize >> 8));
    // The last piece is shorter than 1300 (empty when len is a multiple of 1300).
    const auto& last = w.packets.back();
    CHECK(int(last.size()) - 10 == len % kFragmentSize);
    CHECK(tx.outgoingSequence() == 2);

    std::vector<uint8_t> got;
    for (size_t i = 0; i + 1 < w.packets.size(); ++i) CHECK(!deliver(rx, w.packets[i], got));
    CHECK(deliver(rx, w.packets.back(), got));
    CHECK(got == msg);
    CHECK(rx.incomingSequence() == 1);
}

void testNetchanLostFragment() {
    Wire w;
    Netchan tx(NetSide::Server, 0, w.fn());
    Netchan rx(NetSide::Client, 0, nullptr);
    std::vector<uint8_t> msg(3000, 9);
    tx.transmit(msg.data(), 3000);
    while (tx.hasPendingFragments()) tx.transmitNextFragment();
    CHECK(w.packets.size() == 3);
    std::vector<uint8_t> got;
    CHECK(!deliver(rx, w.packets[0], got));
    CHECK(!deliver(rx, w.packets[2], got));  // piece 1 missing: dropped
    CHECK(rx.incomingSequence() == 0);
    // The next message (sequence 2) still gets through; the lost sequence 1 counts as one dropped packet.
    const uint8_t small[] = {5};
    tx.transmit(small, 1);
    CHECK(deliver(rx, w.packets.back(), got));
    CHECK(got.size() == 1 && got[0] == 5);
    CHECK(rx.dropped() == 1);
}

void testTokenizeAndInfo() {
    auto t = tokenize("connect \"\\protocol\\118\\name\\a b\" // tail");
    CHECK(t.size() == 2);
    CHECK(t[0] == "connect");
    CHECK(t[1] == "\\protocol\\118\\name\\a b");
    CHECK(infoValueForKey(t[1], "name") == "a b");
    CHECK(infoValueForKey(t[1], "protocol") == "118");
    CHECK(infoValueForKey(t[1], "missing").empty());
    auto q = tokenize("say \"a\\\"b\" c");
    CHECK(q.size() == 3 && q[1] == "a\"b" && q[2] == "c");  // an escaped quote stays in the token
    auto t2 = tokenize("  getinfo   xxx/*c*/yy ");
    CHECK(t2.size() == 3 && t2[1] == "xxx" && t2[2] == "yy");
    std::string info;
    infoSetValueForKey(info, "a", "1");
    infoSetValueForKey(info, "b", "2");
    infoSetValueForKey(info, "a", "3");
    CHECK(info == "\\b\\2\\a\\3");
}

void testOutOfBand() {
    auto pkt = makeOutOfBand("getinfo 12%3\nrest");
    CHECK(pkt.size() == 4 + 17);  // no terminating zero
    auto line = parseOutOfBand(pkt.data(), int(pkt.size()));
    CHECK(line && *line == "getinfo 12.3");
    const uint8_t notOob[] = {1, 0, 0, 0, 'x'};
    CHECK(!parseOutOfBand(notOob, 5));
}

void testServerReplies() {
    ConnectionlessServer srv(42);
    srv.time = 1000;
    srv.info.hostname = "test";
    srv.info.mapname = "mp_toujane";
    srv.info.gametype = "tdm";
    srv.info.maxClients = 16;
    srv.info.statusCvars = {{"sv_hostname", "test"}, {"mapname", "mp_toujane"}};
    srv.info.players = {{5, 48, "Alice"}, {0, 999, "Bob"}};
    ConnectionlessServer::Address from{0x7f000001, 28961};

    auto info = srv.handle(from, "getinfo abc");
    CHECK(info && *info ==
                      "infoResponse\n\\challenge\\abc\\protocol\\118\\hostname\\test\\mapname\\mp_toujane"
                      "\\sv_maxclients\\16\\gametype\\tdm\\hw\\1\\mod\\0\\voice\\0\\pb\\0");
    auto status = srv.handle(from, "getstatus xyz");
    CHECK(status && *status ==
                        "statusResponse\n\\sv_hostname\\test\\mapname\\mp_toujane\\challenge\\xyz\\pswrd\\0\\mod\\0\n"
                        "5 48 \"Alice\"\n0 999 \"Bob\"\n");
    CHECK(!srv.handle(from, "pb_sv_whatever"));
    CHECK(!srv.handle(from, "disconnect"));

    auto ch = srv.handle(from, "getchallenge");
    CHECK(ch && ch->rfind("challengeResponse ", 0) == 0);
    int challenge = std::atoi(ch->c_str() + 18);
    CHECK(challenge == srv.challengeFor(from));
    auto ch2 = srv.handle(from, "getchallenge");
    CHECK(ch2 && *ch2 == *ch);  // same address keeps its challenge

    auto wrongProto = srv.handle(from, "connect \"\\protocol\\117\\challenge\\1\"");
    CHECK(wrongProto && *wrongProto == std::string("error\nEXE_SERVER_IS_DIFFERENT_VER\x15") + "1.3\n");
    auto badChallenge = srv.handle(from, "connect \"\\protocol\\118\\challenge\\1\\qport\\5\"");
    CHECK(badChallenge && *badChallenge == "error\nEXE_BAD_CHALLENGE");
    std::optional<ConnectionlessServer::ConnectRequest> acc;
    auto ok = srv.handle(from, "connect \"\\protocol\\118\\challenge\\" + std::to_string(challenge) +
                                   "\\qport\\4321\\name\\x\"", &acc);
    CHECK(ok && *ok == "connectResponse");
    CHECK(acc && acc->qport == 4321 && acc->challenge == challenge);

    // Challenges issued at server time 0 to two addresses: the second must not evict the first.
    ConnectionlessServer early(43);
    early.time = 0;
    ConnectionlessServer::Address p1{0x7f000001, 1000}, p2{0x7f000001, 1001};
    CHECK(early.handle(p1, "getchallenge"));
    const int c1 = early.challengeFor(p1);
    CHECK(early.handle(p2, "getchallenge"));
    CHECK(early.challengeFor(p1) == c1 && early.challengeFor(p2) != 0);
    auto ok1 = early.handle(p1, "connect \"\\protocol\\118\\challenge\\" + std::to_string(c1) + "\\qport\\1\"");
    CHECK(ok1 && *ok1 == "connectResponse");
}

void testUsercmd() {
    const uint32_t key = 0x5a5a1234u;
    UserCmd a;
    a.serverTime = 1000;
    a.buttons = 0x41;
    a.weapon = 3;
    a.angles[0] = 100;
    a.angles[1] = 0xfff0;
    a.forwardmove = 127;

    struct Case { UserCmd to; };
    std::vector<UserCmd> cases;
    UserCmd same = a; same.serverTime = 1050; cases.push_back(same);
    UserCmd minor = same; minor.angles[0] = 200; minor.buttons ^= 1; minor.rightmove = -127; cases.push_back(minor);
    UserCmd major = minor; major.serverTime = 100000; major.weapon = 7; major.offhand = 2; major.buttons = 0x7ff01;
    major.angles[2] = 0x8000; cases.push_back(major);

    for (const UserCmd& to : cases) {
        uint8_t buf[128];
        Msg m(buf, sizeof buf);
        writeDeltaUsercmd(m, key, a, to);
        m.beginReading();
        UserCmd back;
        readDeltaUsercmd(m, key, a, back);
        CHECK(!m.overflowed());
        CHECK(back.serverTime == to.serverTime);
        CHECK(back.buttons == to.buttons);
        CHECK(back.weapon == to.weapon && back.offhand == to.offhand);
        for (int i = 0; i < 3; ++i) CHECK(back.angles[i] == (to.angles[i] & 0xffff));
        CHECK(back.forwardmove == to.forwardmove && back.rightmove == to.rightmove);
    }

    // Unchanged command: 1 bit + 1 time byte + 1 keyed bit, all in 2 bytes.
    uint8_t buf[16];
    Msg m(buf, sizeof buf);
    writeDeltaUsercmd(m, key, a, same);
    CHECK(m.cursize() == 2);
    CHECK(buf[1] == 50);
    CHECK(buf[0] == (1 | ((key & 1) << 1)));

    // Small movement is quantised to the 4-bit class.
    CHECK(moveClass(10, -10) == 0);
    CHECK(moveClass(11, -11) == (1 | 8));
    int8_t f, r;
    moveFromClass(2 | 4, f, r);
    CHECK(f == -127 && r == 127);
}

struct TestState {
    int32_t number;
    float origin[3];
    float angle;
    float small;
    int32_t flags;
    int32_t eType;
    int32_t neg;
};

void testFieldsAndFrames() {
    static const NetField fields[] = {
        {"origin0", offsetof(TestState, origin) + 0, kFieldFloat},
        {"origin1", offsetof(TestState, origin) + 4, kFieldFloat},
        {"origin2", offsetof(TestState, origin) + 8, kFieldFloat},
        {"angle", offsetof(TestState, angle), kFieldAngle},
        {"small", offsetof(TestState, small), kFieldSmallFloat},
        {"flags", offsetof(TestState, flags), 19},
        {"eType", offsetof(TestState, eType), 8},
        {"neg", offsetof(TestState, neg), 32},
    };
    TestState from{};
    TestState to{};
    to.origin[0] = 123.0f;     // integral shortcut
    to.origin[1] = -4096.0f;   // lowest shortcut value
    to.origin[2] = 0.5f;       // raw float
    to.angle = 90.0f;
    to.small = 511.0f;
    to.flags = 0x7ffff;
    to.eType = 0;              // unchanged (equal)
    to.neg = -5;

    uint8_t buf[256];
    Msg m(buf, sizeof buf);
    writeDeltaStruct(m, reinterpret_cast<const uint8_t*>(&from), reinterpret_cast<const uint8_t*>(&to), false,
                     fields, 8, 77, 10, false);
    m.beginReading();
    CHECK(m.readBits(10) == 77);
    TestState back{};
    StructDelta d = readDeltaStructBody(m, reinterpret_cast<const uint8_t*>(&from), reinterpret_cast<uint8_t*>(&back),
                                        sizeof back, fields, 8);
    CHECK(d == StructDelta::Changed);
    CHECK(back.origin[0] == 123.0f && back.origin[1] == -4096.0f && back.origin[2] == 0.5f);
    CHECK(back.angle > 89.99f && back.angle < 90.01f);
    CHECK(back.small == 511.0f);
    CHECK(back.flags == 0x7ffff);
    CHECK(back.neg == -5);
    CHECK(!m.overflowed());

    // Integral float shortcut byte layout: 1 (changed) 1 (non-zero) 0 (shortcut) then 5 low bits, one byte.
    {
        static const NetField one[] = {{"x", 0, kFieldFloat}};
        float f0 = 0.0f, f1 = 1.0f;
        uint8_t b[16];
        Msg mm(b, sizeof b);
        writeDeltaField(mm, reinterpret_cast<const uint8_t*>(&f0), reinterpret_cast<const uint8_t*>(&f1), one[0]);
        // value + 4096 = 4097 = 0b1_0000_0000_0001 -> low 5 bits 00001, byte 128.
        CHECK(mm.cursize() == 2);
        CHECK(b[0] == (1 | 2 | 0 | (1 << 3)));
        CHECK(b[1] == 128);
    }

    // Unchanged without force writes nothing; with force: number, 0, 0.
    Msg u(buf, sizeof buf);
    writeDeltaStruct(u, reinterpret_cast<const uint8_t*>(&from), reinterpret_cast<const uint8_t*>(&from), false,
                     fields, 8, 1, 10, false);
    CHECK(u.cursize() == 0);
    writeDeltaStruct(u, reinterpret_cast<const uint8_t*>(&from), reinterpret_cast<const uint8_t*>(&from), true,
                     fields, 8, 1, 10, false);
    CHECK(u.bitLength() == 12);
    // Removal with the client-state leading bit.
    Msg r(buf, sizeof buf);
    writeDeltaStruct(r, reinterpret_cast<const uint8_t*>(&from), nullptr, false, fields, 8, 3, 6, true);
    r.beginReading();
    CHECK(r.readBit() == 1);
    CHECK(r.readBits(6) == 3);
    CHECK(readDeltaStructBody(r, reinterpret_cast<const uint8_t*>(&from), reinterpret_cast<uint8_t*>(&back),
                              sizeof back, fields, 8) == StructDelta::Removed);
}

} // namespace

void testScrambleAndHash() {
    uint8_t d[6] = {0, 1, 2, 3, 4, 5};
    scramble(d, 6, 0x5a, "abc");
    CHECK(hex(d, 6) == "3bfe9e5d38ff");  // key 0x5a ^ 'a' = 0x3b, then ^ ('b' << 1) = 0xff, ...
    scramble(d, 6, 0x5a, "abc");
    CHECK(hex(d, 6) == "000102030405");
    uint8_t e[3] = {7, 7, 7};
    scramble(e, 3, 0x10, "");  // an empty string keeps the key
    CHECK(hex(e, 3) == "171717");
    CHECK(serverToClientKey(0x12345678, 0x100) == 0x78);
    CHECK(clientToServerKey(0x0f, 0xf0, 0x101) == 0xfe);
    CHECK(commandHashKey("") == 0);
    CHECK(commandHashKey("ab") == 23313);
    CHECK(commandHashKey("v cg_fovscale \"1\"") == 182384);
    CHECK(commandHashKey(std::string(40, 'x')) == commandHashKey(std::string(32, 'x')));
}

int32_t ldi(const std::vector<uint8_t>& v, int off) {
    int32_t x;
    std::memcpy(&x, v.data() + off, 4);
    return x;
}
void sti(std::vector<uint8_t>& v, int off, int32_t x) { std::memcpy(v.data() + off, &x, 4); }
void stf(std::vector<uint8_t>& v, int off, float f) { std::memcpy(v.data() + off, &f, 4); }

void testNetfieldTables() {
    struct T { const NetField* f; int n; int size; };
    const T tables[] = {{kEntityStateFields, 59, kEntityStateSize}, {kArchivedEntityFields, 68, kArchivedEntitySize},
                        {kClientStateFields, 22, kClientStateSize}, {kPlayerStateFields, 105, kPlayerStateSize},
                        {kObjectiveFields, 6, kObjectiveSize}, {kHudElemFields, 32, kHudElemSize}};
    for (const T& t : tables)
        for (int i = 0; i < t.n; ++i) {
            CHECK(t.f[i].offset >= 0 && t.f[i].offset + 4 <= t.size);
            CHECK(t.f[i].offset % 4 == 0);
        }
    CHECK(std::string(kEntityStateFields[0].name) == "pos.trTime");
    CHECK(kPlayerStateFields[9].bits == -8);  // movementDir: signed 8 bits
}

void testPlayerStateRoundTrip() {
    std::vector<uint8_t> a(kPlayerStateSize, 0), b(kPlayerStateSize, 0), out(kPlayerStateSize, 0);
    sti(b, 0, 123456);         // commandTime
    stf(b, 20, -170.0f);       // origin[0]: integral float
    stf(b, 24, 1008.25f);      // origin[1]: raw float
    stf(b, 28, -4096.0f);      // origin[2]: lowest short form
    stf(b, 236, 90.0f);        // viewangles[1]: angle
    sti(b, 156, -3);           // movementDir: signed 8 bits
    sti(b, 52, -200);          // weaponTime: signed 16 bits
    sti(b, kPsStatsOffset + 0, 100);
    sti(b, kPsStatsOffset + 12, 33);  // stat 3: 6 bits
    sti(b, kPsStatsOffset + 20, 200); // stat 5: byte
    sti(b, kPsAmmoOffset + 4 * 17, 90);
    sti(b, kPsAmmoClipOffset + 4 * 63, 30);
    sti(b, kPsObjectivesOffset + kObjectiveSize, 2);  // objective 1 state
    stf(b, kPsObjectivesOffset + kObjectiveSize + 4, 512.0f);
    sti(b, kPsHudElemsOffset, 1);       // type
    stf(b, kPsHudElemsOffset + 4, 320.0f);
    sti(b, kPsArchivalHudElemsOffset, 2);
    sti(b, kPsArchivalHudElemsOffset + 104, 5000);
    static uint8_t buf[0x4000];
    Msg m(buf, sizeof buf);
    writeDeltaPlayerState(m, a.data(), b.data());
    CHECK(!m.overflowed());
    m.beginReading();
    readDeltaPlayerState(m, a.data(), out.data());
    CHECK(!m.overflowed());
    CHECK(std::memcmp(out.data(), b.data(), 300) == 0);
    CHECK(std::memcmp(out.data() + 300, b.data() + 300, kPlayerStateSize - 300) == 0);
    CHECK(ldi(out, 156) == -3 && ldi(out, 52) == -200);
    // Unchanged state: count byte 0 and five section bits / groups, all zero.
    Msg u(buf, sizeof buf);
    writeDeltaPlayerState(u, b.data(), b.data());
    CHECK(u.cursize() == 2);
    CHECK(buf[0] == 0 && buf[1] == 0);
    // Delta against a base: only the changed field travels.
    std::vector<uint8_t> c = b;
    sti(c, 0, 123506);
    Msg d(buf, sizeof buf);
    writeDeltaPlayerState(d, b.data(), c.data());
    d.beginReading();
    readDeltaPlayerState(d, b.data(), out.data());
    CHECK(std::memcmp(out.data(), c.data(), kPlayerStateSize) == 0);
    CHECK(buf[0] == 1);  // lc: only commandTime
    // A player-state integral float has no zero shortcut: 0 bit, then 5 bits and a byte.
    std::vector<uint8_t> z(kPlayerStateSize, 0);
    stf(z, 20, 0.0f);
    std::vector<uint8_t> one(kPlayerStateSize, 0);
    stf(one, 20, 1.0f);
    Msg f(buf, sizeof buf);
    writeDeltaPlayerState(f, one.data(), z.data());  // origin[0] 1.0 -> 0.0
    CHECK(buf[0] == 3);  // lc: origin[0] is the third field
    f.beginReading();
    readDeltaPlayerState(f, one.data(), out.data());
    CHECK(ldi(out, 20) == 0);
}

void testFieldList() {
    std::vector<uint8_t> a(kObjectiveSize, 0), b(kObjectiveSize, 0), out(kObjectiveSize, 0);
    uint8_t buf[256];
    Msg m(buf, sizeof buf);
    writeDeltaFieldList(m, a.data(), b.data(), kObjectiveFields, 6);
    CHECK(m.bitLength() == 1);  // nothing changed: one 0 bit
    stf(b, 8, 100.0f);
    sti(b, 16, 7);
    Msg n(buf, sizeof buf);
    writeDeltaFieldList(n, a.data(), b.data(), kObjectiveFields, 6);
    n.beginReading();
    readDeltaFieldList(n, a.data(), out.data(), kObjectiveFields, 6);
    CHECK(std::memcmp(out.data(), b.data(), kObjectiveSize) == 0);
}

void testUsercmdBase() {
    std::vector<uint8_t> ps(kPlayerStateSize, 0);
    ps[212] = 9;
    ps[208] = 3;
    stf(ps, 232, 90.0f);   // viewangles[0]
    stf(ps, 236, -45.0f);  // viewangles[1]
    sti(ps, 84, 100);      // delta_angles[0]
    sti(ps, 12, 0x800000);
    sti(ps, 160, 4);
    stf(ps, 76, -0.5f);
    UserCmd c = usercmdBase(ps.data());
    CHECK(c.weapon == 9 && c.offhand == 3);
    CHECK(c.angles[0] == ((16384 - 100) & 0xffff));
    CHECK(c.angles[1] == ((-8192) & 0xffff));
    CHECK(c.angles[2] == 0);
    CHECK(c.buttons == (0x200 | 0x40));
}

int main() {
    testScrambleAndHash();
    testNetfieldTables();
    testPlayerStateRoundTrip();
    testFieldList();
    testUsercmdBase();
    testMsgMixedPacking();
    testMsgOverflowAndStrings();
    testHuffmanCodes();
    testHuffmanVectors();
    testNetchanSmall();
    testNetchanFragments(3000);
    testNetchanFragments(2600);  // multiple of 1300: ends with an empty piece
    testNetchanFragments(1300);
    testNetchanLostFragment();
    testTokenizeAndInfo();
    testOutOfBand();
    testServerReplies();
    testUsercmd();
    testFieldsAndFrames();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "net: all tests passed\n";
    return 0;
}
