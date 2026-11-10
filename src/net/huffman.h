// Static Huffman coder of the game messages, implemented from the spec (docs/spec/net.md section 3).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iw2::net {

class Huffman {
public:
    // Builds the tree from the protocol's frequency table (huffman_table.cpp).
    static const Huffman& protocol();

    // Builds a tree by adaptive insertion of every byte value v = 0..255, freq[v] times.
    explicit Huffman(const int32_t (&freq)[256]);

    // Writes the codes of in[0..len) LSB-first into out starting at bit 0; returns the byte count (bits + 7) / 8.
    int compress(const uint8_t* in, int len, uint8_t* out, int outCapacity) const;
    // Decodes while the bit position is below 8 * len; may yield trailing bytes from the padding bits.
    int decompress(const uint8_t* in, int len, uint8_t* out, int outCapacity) const;

    // Root-to-leaf path of a byte value as '0'/'1' characters.
    std::string code(int symbol) const;

private:
    static constexpr int kNyt = 256;
    static constexpr int kInternal = 257;
    static constexpr int kNone = -1;

    struct Node {
        int left = kNone, right = kNone, parent = kNone;
        int next = kNone, prev = kNone;  // weight-ordered list
        int head = kNone;                 // index of the shared block-leader cell
        int weight = 0;
        int symbol = 0;
    };

    int newCell();
    void freeCell(int cell);
    void swapTree(int a, int b);
    void swapList(int a, int b);
    void increment(int node);
    void addRef(int symbol);

    std::vector<Node> nodes_;
    std::vector<int> cells_;      // leader cells: node index, or next free cell when on the free list
    int freeList_ = kNone;
    int tree_ = kNone;
    int lhead_ = kNone;
    int loc_[257];
};

} // namespace iw2::net
