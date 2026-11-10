// An independent decoder of the server-to-client stream, written from docs/spec/net.md sections 2-7 (the client's
// side of the protocol) without using openiw2's own coders: its bit reader, Huffman tree, unscrambling, netchan,
// gamestate, snapshot and delta readers are separate code. Only data constants are shared (the Huffman byte counts
// and the field tables, which the spec names as interoperability tables). Used by the verification tests to check
// that a stream is acceptable to a client built from the spec, whichever server produced it.
#include "specdecoder.h"

#include <cmath>
#include <cstring>

#include "net/netfields.h"

namespace iw2::net {
extern const int32_t kHuffmanFrequencies[256];
}

namespace specdec {

namespace {

// ---- section 3: the adaptive Huffman tree built from the byte counts --------------------------------------------
struct Node {
    Node *left = nullptr, *right = nullptr, *parent = nullptr, *next = nullptr, *prev = nullptr;
    Node** head = nullptr;
    int weight = 0;
    int symbol = 0;
};

struct Tree {
    static constexpr int kNyt = 256, kInternal = 257;
    Node nodes[768];
    Node* ptrs[768];
    int nextNode = 0, nextPtr = 0;
    Node** freeList = nullptr;
    Node* root = nullptr;
    Node* listHead = nullptr;
    Node* loc[257] = {};

    Node** getPtr() {
        if (!freeList) return &ptrs[nextPtr++];
        Node** p = freeList;
        freeList = reinterpret_cast<Node**>(*p);
        return p;
    }
    void freePtr(Node** p) {
        *p = reinterpret_cast<Node*>(freeList);
        freeList = p;
    }
    void swapTree(Node* a, Node* b) {
        Node* pa = a->parent;
        Node* pb = b->parent;
        if (pa) (pa->left == a ? pa->left : pa->right) = b;
        else root = b;
        if (pb) (pb->left == b ? pb->left : pb->right) = a;
        else root = a;
        a->parent = pb;
        b->parent = pa;
    }
    static void swapList(Node* a, Node* b) {
        Node* t = a->next;
        a->next = b->next;
        b->next = t;
        t = a->prev;
        a->prev = b->prev;
        b->prev = t;
        if (a->next == a) a->next = b;
        if (b->next == b) b->next = a;
        if (a->next) a->next->prev = a;
        if (b->next) b->next->prev = b;
        if (a->prev) a->prev->next = a;
        if (b->prev) b->prev->next = b;
    }
    void increment(Node* n) {
        if (!n) return;
        if (n->next && n->next->weight == n->weight) {
            Node* leader = *n->head;
            if (leader != n->parent) swapTree(leader, n);
            swapList(leader, n);
        }
        if (n->prev && n->prev->weight == n->weight) {
            *n->head = n->prev;
        } else {
            *n->head = nullptr;
            freePtr(n->head);
        }
        ++n->weight;
        if (n->next && n->next->weight == n->weight) {
            n->head = n->next->head;
        } else {
            n->head = getPtr();
            *n->head = n;
        }
        if (n->parent) {
            increment(n->parent);
            if (n->prev == n->parent) {
                swapList(n, n->parent);
                if (*n->head == n) *n->head = n->parent;
            }
        }
    }
    void add(int ch) {
        if (loc[ch]) {
            increment(loc[ch]);
            return;
        }
        Node* leaf = &nodes[nextNode++];
        Node* inner = &nodes[nextNode++];
        inner->symbol = kInternal;
        inner->weight = 1;
        inner->next = listHead->next;
        if (listHead->next) {
            listHead->next->prev = inner;
            if (listHead->next->weight == 1) {
                inner->head = listHead->next->head;
            } else {
                inner->head = getPtr();
                *inner->head = inner;
            }
        } else {
            inner->head = getPtr();
            *inner->head = inner;
        }
        listHead->next = inner;
        inner->prev = listHead;
        leaf->symbol = ch;
        leaf->weight = 1;
        leaf->next = listHead->next;
        if (listHead->next) {
            listHead->next->prev = leaf;
            if (listHead->next->weight == 1) {
                leaf->head = listHead->next->head;
            } else {
                leaf->head = getPtr();
                *leaf->head = inner;
            }
        } else {
            leaf->head = getPtr();
            *leaf->head = leaf;
        }
        listHead->next = leaf;
        leaf->prev = listHead;
        if (listHead->parent) (listHead->parent->left == listHead ? listHead->parent->left : listHead->parent->right) = inner;
        else root = inner;
        inner->right = leaf;
        inner->left = listHead;
        inner->parent = listHead->parent;
        listHead->parent = leaf->parent = inner;
        loc[ch] = leaf;
        increment(inner->parent);
    }
    Tree() {
        root = listHead = loc[kNyt] = &nodes[nextNode++];
        root->symbol = kNyt;
        for (int v = 0; v < 256; ++v)
            for (int k = 0; k < iw2::net::kHuffmanFrequencies[v]; ++k) add(v);
    }
};

const Tree& tree() {
    static const Tree t;
    return t;
}

std::vector<uint8_t> decompress(const uint8_t* in, int len) {
    std::vector<uint8_t> out;
    const Node* rootNode = tree().root;
    long bit = 0;
    const long total = long(len) * 8;
    while (bit < total) {
        const Node* n = rootNode;
        while (n->symbol == Tree::kInternal) {
            const int b = bit < total ? (in[bit >> 3] >> (bit & 7)) & 1 : 0;
            ++bit;
            n = b ? n->right : n->left;
        }
        out.push_back(n->symbol == Tree::kNyt ? 0 : uint8_t(n->symbol));
    }
    return out;
}

// ---- section 2: the mixed bit / byte reader -----------------------------------------------------------------------
struct Reader {
    const uint8_t* d;
    int size;
    int read = 0;
    int bit = 0;
    int bitByte = 0;
    bool overflow = false;

    int bits(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i) {
            if ((bit & 7) == 0) {
                if (read >= size) {
                    overflow = true;
                    return -1;
                }
                bitByte = read++;
                bit = bitByte * 8;
            }
            v |= ((d[bitByte] >> (bit & 7)) & 1) << i;
            ++bit;
        }
        return v;
    }
    int byte() {
        if (read >= size) {
            overflow = true;
            return -1;
        }
        return d[read++];
    }
    int shortv() {
        if (read + 2 > size) {
            overflow = true;
            return -1;
        }
        const int16_t v = int16_t(d[read] | d[read + 1] << 8);
        read += 2;
        return v;
    }
    int32_t longv() {
        if (read + 4 > size) {
            overflow = true;
            return -1;
        }
        int32_t v;
        std::memcpy(&v, d + read, 4);
        read += 4;
        return v;
    }
    std::string string() {
        std::string s;
        for (;;) {
            const int c = byte();
            if (c <= 0) break;
            s.push_back(c == 0x92 ? '\'' : char(c));
        }
        return s;
    }
};

void putI(uint8_t* s, int off, int32_t v) { std::memcpy(s + off, &v, 4); }
void putF(uint8_t* s, int off, float v) { std::memcpy(s + off, &v, 4); }

bool isFloatCode(int b) { return b == 0 || b == -99 || b == -100; }

// Section 7 field coder (entity, client, objective and HUD fields): with the zero shortcut.
void readField(Reader& r, const uint8_t* from, uint8_t* to, const iw2::net::NetField& f) {
    if (r.bits(1) == 0) {
        std::memcpy(to + f.offset, from + f.offset, 4);
        return;
    }
    if (f.bits == -100) {
        putF(to, f.offset, r.bits(1) == 0 ? 0.0f : float(r.shortv()) * 360.0f / 65536.0f);
        return;
    }
    if (f.bits == 0 || f.bits == -99) {
        if (r.bits(1) == 0) {
            putF(to, f.offset, 0.0f);
            return;
        }
        if (r.bits(1) == 0) {
            const int lowBits = f.bits == 0 ? 5 : 2, bias = f.bits == 0 ? 4096 : 512;
            const int low = r.bits(lowBits);
            const int high = r.byte();
            putF(to, f.offset, float((low | high << lowBits) - bias));
        } else {
            const int32_t raw = r.longv();
            std::memcpy(to + f.offset, &raw, 4);
        }
        return;
    }
    if (r.bits(1) == 0) {
        putI(to, f.offset, 0);
        return;
    }
    int n = f.bits < 0 ? -f.bits : f.bits;
    uint32_t v = 0;
    int shift = 0;
    if (n & 7) {
        v = uint32_t(r.bits(n & 7)) & ((1u << (n & 7)) - 1);
        shift = n & 7;
        n -= n & 7;
    }
    for (; n > 0; n -= 8, shift += 8) v |= uint32_t(r.byte() & 0xff) << shift;
    putI(to, f.offset, int32_t(v));
}

// Section 7.3 field coder of the player state: no zero shortcut; negative integer codes are signed.
void readPsField(Reader& r, const uint8_t* from, uint8_t* to, const iw2::net::NetField& f) {
    if (r.bits(1) == 0) {
        std::memcpy(to + f.offset, from + f.offset, 4);
        return;
    }
    if (f.bits == -100) {
        putF(to, f.offset, r.bits(1) == 0 ? 0.0f : float(r.shortv()) * 360.0f / 65536.0f);
        return;
    }
    if (isFloatCode(f.bits)) {
        if (r.bits(1) == 0) {
            const int lowBits = f.bits == 0 ? 5 : 2, bias = f.bits == 0 ? 4096 : 512;
            const int low = r.bits(lowBits);
            const int high = r.byte();
            putF(to, f.offset, float((low | high << lowBits) - bias));
        } else {
            const int32_t raw = r.longv();
            std::memcpy(to + f.offset, &raw, 4);
        }
        return;
    }
    const int total = f.bits < 0 ? -f.bits : f.bits;
    int n = total;
    uint32_t v = 0;
    int shift = 0;
    if (n & 7) {
        v = uint32_t(r.bits(n & 7)) & ((1u << (n & 7)) - 1);
        shift = n & 7;
        n -= n & 7;
    }
    for (; n > 0; n -= 8, shift += 8) v |= uint32_t(r.byte() & 0xff) << shift;
    int32_t s = int32_t(v);
    if (f.bits < 0 && total < 32 && (v >> (total - 1)) & 1) s = int32_t(v | ~((1u << total) - 1));
    putI(to, f.offset, s);
}

// Section 7 structure frame body after the number: removed bit, changed bit, lc byte, fields.
enum class Body { Removed, Unchanged, Changed };
Body readBody(Reader& r, const uint8_t* from, uint8_t* to, int size, const iw2::net::NetField* fields, int count) {
    std::memcpy(to, from, size_t(size));
    if (r.bits(1) == 1) return Body::Removed;
    if (r.bits(1) == 0) return Body::Unchanged;
    const int lc = r.byte();
    for (int i = 0; i < lc && i < count; ++i) readField(r, from, to, fields[i]);
    return Body::Changed;
}

void readAmmoGroups(Reader& r, uint8_t* to, int base) {
    for (int g = 0; g < 4; ++g) {
        if (r.bits(1) == 0) continue;
        const int mask = r.shortv() & 0xffff;
        for (int i = 0; i < 16; ++i)
            if (mask & (1 << i)) putI(to, base + 4 * (16 * g + i), r.shortv());
    }
}

void readHudArray(Reader& r, const uint8_t* from, uint8_t* to, int base) {
    const int count = r.bits(5);
    for (int i = 0; i < count; ++i) {
        const int off = base + i * iw2::net::kHudElemSize;
        const int last = r.bits(5);
        for (int k = 0; k <= last && k < 32; ++k) readField(r, from + off, to + off, iw2::net::kHudElemFields[k]);
    }
}

void readPlayerState(Reader& r, const uint8_t* from, uint8_t* to) {
    using namespace iw2::net;
    std::memcpy(to, from, size_t(kPlayerStateSize));
    const int lc = r.byte();
    for (int i = 0; i < lc && i < 105; ++i) readPsField(r, from, to, kPlayerStateFields[i]);
    if (r.bits(1)) {  // stats
        const int mask = r.bits(6);
        for (int i = 0; i < 6; ++i) {
            if (!(mask & (1 << i))) continue;
            const int v = i == 3 ? r.bits(6) : i == 5 ? r.byte() : r.shortv();
            putI(to, 300 + 4 * i, v);
        }
    }
    if (r.bits(1)) readAmmoGroups(r, to, 324);  // ammo
    readAmmoGroups(r, to, 836);                 // ammo in clip: the groups without a section bit
    if (r.bits(1)) {  // objectives
        for (int o = 0; o < 16; ++o) {
            const int off = 1508 + o * kObjectiveSize;
            putI(to, off, r.bits(3));
            if (r.bits(1))
                for (const NetField& f : kObjectiveFields) readField(r, from + off, to + off, f);
        }
    }
    if (r.bits(1)) {  // HUD elements: the array at +5928, then the one at +1960
        readHudArray(r, from, to, 5928);
        readHudArray(r, from, to, 1960);
    }
}

void unscramble(uint8_t* d, int n, uint8_t key, const std::string& s) {
    size_t p = 0;
    for (int i = 0; i < n; ++i) {
        if (!s.empty()) {
            key = uint8_t(key ^ ((uint8_t(s[p]) << (i & 1)) & 0xff));
            if (++p >= s.size()) p = 0;
        }
        d[i] ^= key;
    }
}

}  // namespace

bool Decoder::packet(const uint8_t* data, int len) {
    if (len < 4) return false;
    std::vector<uint8_t> msg;
    int32_t seq;
    std::memcpy(&seq, data, 4);
    int start = 4;
    if (seq & 0x80000000) {
        if (len < 10) return false;
        int32_t fragStart;
        std::memcpy(&fragStart, data + 4, 4);
        const int fragLen = int16_t(data[8] | data[9] << 8);
        seq &= 0x7fffffff;
        if (seq != fragSeq_) {
            fragSeq_ = seq;
            frag_.clear();
        }
        if (fragStart != int(frag_.size()) || fragLen < 0 || 10 + fragLen > len) return true;
        frag_.insert(frag_.end(), data + 10, data + 10 + fragLen);
        if (fragLen == 1300) return true;
        msg = frag_;
        frag_.clear();
    } else {
        msg.assign(data + start, data + len);
    }
    if (seq <= incoming_) return true;
    incoming_ = seq;
    if (msg.size() < 4) return false;
    int32_t lead;
    std::memcpy(&lead, msg.data(), 4);
    unscramble(msg.data() + 4, int(msg.size()) - 4, uint8_t((challenge ^ seq) & 0xff),
               clientCommandText ? clientCommandText(lead) : std::string());
    std::vector<uint8_t> body = decompress(msg.data() + 4, int(msg.size()) - 4);
    Reader r{body.data(), int(body.size())};
    for (;;) {
        const int op = r.byte();
        if (r.overflow) return error("message without end");
        if (op == 7) return true;
        if (op == 0) continue;
        if (op == 4) {
            const int s = r.longv();
            std::string text = r.string();
            if (s > commandSequence_) {
                commandSequence_ = s;
                commands[s & 127] = text;
                ++commandCount;
            }
        } else if (op == 1) {
            if (!gamestate(&r)) return false;
            return true;
        } else if (op == 6) {
            if (!snapshot(&r, seq)) return false;
        } else {
            return error("illegible server message " + std::to_string(op));
        }
    }
}

bool Decoder::error(const std::string& e) {
    if (err.empty()) err = e;
    return false;
}

bool Decoder::gamestate(void* rp) {
    Reader& r = *static_cast<Reader*>(rp);
    commandSequence_ = r.longv();
    std::vector<uint8_t> zero(iw2::net::kEntityStateSize, 0);
    for (;;) {
        const int op = r.byte();
        if (r.overflow) return error("gamestate overflow");
        if (op == 7) break;
        if (op == 2) {
            const int idx = r.shortv();
            if (idx < 0 || idx > 2047) return error("bad configstring index");
            std::string s;
            for (;;) {
                const int c = r.byte();
                if (c <= 0) break;
                s.push_back(c == '%' ? '.' : c == 0x92 ? '\'' : char(c));
            }
            configstrings[idx] = s;
        } else if (op == 3) {
            const int n = r.bits(10) & 1023;
            std::vector<uint8_t> st(iw2::net::kEntityStateSize);
            readBody(r, zero.data(), st.data(), iw2::net::kEntityStateSize, iw2::net::kEntityStateFields, 59);
            baselines[n] = st;
        } else {
            return error("bad gamestate entry " + std::to_string(op));
        }
    }
    clientNum = r.longv();
    checksumFeed = r.longv();
    haveGamestate = true;
    return !r.overflow || error("gamestate overflow");
}

bool Decoder::snapshot(void* rp, int messageNum) {
    using namespace iw2::net;
    Reader& r = *static_cast<Reader*>(rp);
    Frame f;
    f.messageNum = messageNum;
    f.serverTime = r.longv();
    const int deltaNum = r.byte();
    f.snapFlags = r.byte();
    const Frame* old = nullptr;
    if (deltaNum) {
        auto it = frames.find(messageNum - deltaNum);
        if (it == frames.end()) return error("delta frame missing");
        old = &it->second;
    }
    static const std::vector<uint8_t> zps(kPlayerStateSize, 0), zes(kEntityStateSize, 0), zcs(kClientStateSize, 0);
    f.ps.resize(kPlayerStateSize);
    readPlayerState(r, old ? old->ps.data() : zps.data(), f.ps.data());
    if (old) f.entities = old->entities;
    for (;;) {
        const int n = r.bits(10) & 1023;
        if (r.overflow) return error("entity list overflow");
        if (n == 1023) break;
        const std::vector<uint8_t>* base = &zes;
        if (old && old->entities.count(n)) base = &old->entities.at(n);
        else if (baselines.count(n)) base = &baselines.at(n);
        std::vector<uint8_t> st(kEntityStateSize);
        if (readBody(r, base->data(), st.data(), kEntityStateSize, kEntityStateFields, 59) == Body::Removed)
            f.entities.erase(n);
        else
            f.entities[n] = st;
    }
    if (old) f.clients = old->clients;
    while (r.bits(1) == 1) {
        const int n = r.bits(6) & 63;
        const std::vector<uint8_t>* base = old && old->clients.count(n) ? &old->clients.at(n) : &zcs;
        std::vector<uint8_t> st(kClientStateSize);
        if (readBody(r, base->data(), st.data(), kClientStateSize, kClientStateFields, 22) == Body::Removed)
            f.clients.erase(n);
        else
            f.clients[n] = st;
        if (r.overflow) return error("client list overflow");
    }
    if (r.overflow) return error("snapshot overflow");
    frames[messageNum] = f;
    while (frames.size() > 32) frames.erase(frames.begin());
    ++snapshots;
    return true;
}

}  // namespace specdec
