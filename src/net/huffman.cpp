// Implemented from the spec (docs/spec/net.md section 3).
#include "net/huffman.h"

namespace iw2::net {

extern const int32_t kHuffmanFrequencies[256];

const Huffman& Huffman::protocol() {
    static const Huffman h(reinterpret_cast<const int32_t (&)[256]>(kHuffmanFrequencies));
    return h;
}

Huffman::Huffman(const int32_t (&freq)[256]) {
    for (int& l : loc_) l = kNone;
    nodes_.reserve(768);
    nodes_.push_back(Node{});
    Node& nyt = nodes_.back();
    nyt.symbol = kNyt;
    tree_ = lhead_ = loc_[kNyt] = 0;
    for (int v = 0; v < 256; ++v)
        for (int32_t i = 0; i < freq[v]; ++i) addRef(v);
}

int Huffman::newCell() {
    if (freeList_ == kNone) {
        cells_.push_back(kNone);
        return static_cast<int>(cells_.size()) - 1;
    }
    int c = freeList_;
    freeList_ = cells_[c];
    return c;
}

void Huffman::freeCell(int cell) {
    cells_[cell] = freeList_;
    freeList_ = cell;
}

// Exchanges the tree positions of two nodes.
void Huffman::swapTree(int a, int b) {
    int pa = nodes_[a].parent, pb = nodes_[b].parent;
    if (pa != kNone) {
        if (nodes_[pa].left == a) nodes_[pa].left = b; else nodes_[pa].right = b;
    } else {
        tree_ = b;
    }
    if (pb != kNone) {
        if (nodes_[pb].left == b) nodes_[pb].left = a; else nodes_[pb].right = a;
    } else {
        tree_ = a;
    }
    nodes_[a].parent = pb;
    nodes_[b].parent = pa;
}

// Exchanges the positions of two nodes in the weight-ordered list.
void Huffman::swapList(int a, int b) {
    Node& na = nodes_[a];
    Node& nb = nodes_[b];
    std::swap(na.next, nb.next);
    std::swap(na.prev, nb.prev);
    if (na.next == a) na.next = b;
    if (nb.next == b) nb.next = a;
    if (na.next != kNone) nodes_[na.next].prev = a;
    if (nb.next != kNone) nodes_[nb.next].prev = b;
    if (na.prev != kNone) nodes_[na.prev].next = a;
    if (nb.prev != kNone) nodes_[nb.prev].next = b;
}

void Huffman::increment(int node) {
    if (node == kNone) return;
    Node* n = &nodes_[node];
    if (n->next != kNone && nodes_[n->next].weight == n->weight) {
        int leader = cells_[n->head];
        if (leader != n->parent) swapTree(leader, node);
        swapList(leader, node);
    }
    if (n->prev != kNone && nodes_[n->prev].weight == n->weight) {
        cells_[n->head] = n->prev;
    } else {
        cells_[n->head] = kNone;
        freeCell(n->head);
    }
    ++n->weight;
    if (n->next != kNone && nodes_[n->next].weight == n->weight) {
        n->head = nodes_[n->next].head;
    } else {
        n->head = newCell();
        cells_[n->head] = node;
    }
    int parent = n->parent;
    if (parent != kNone) {
        increment(parent);
        n = &nodes_[node];
        if (n->prev == parent) {
            swapList(node, parent);
            if (cells_[n->head] == node) cells_[n->head] = parent;
        }
    }
}

void Huffman::addRef(int symbol) {
    if (loc_[symbol] != kNone) {
        increment(loc_[symbol]);
        return;
    }
    // Split the NYT leaf: a new internal node (left: NYT, right: the new leaf), both of weight 1.
    nodes_.push_back(Node{});
    int leaf = static_cast<int>(nodes_.size()) - 1;
    nodes_.push_back(Node{});
    int inner = static_cast<int>(nodes_.size()) - 1;
    Node& in = nodes_[inner];
    Node& lf = nodes_[leaf];
    Node& lh = nodes_[lhead_];

    in.symbol = kInternal;
    in.weight = 1;
    in.next = lh.next;
    if (lh.next != kNone) {
        nodes_[lh.next].prev = inner;
        if (nodes_[lh.next].weight == 1) {
            in.head = nodes_[lh.next].head;
        } else {
            in.head = newCell();
            cells_[in.head] = inner;
        }
    } else {
        in.head = newCell();
        cells_[in.head] = inner;
    }
    lh.next = inner;
    in.prev = lhead_;

    lf.symbol = symbol;
    lf.weight = 1;
    lf.next = lh.next;
    // lh.next is the inner node of weight 1, so the new leaf joins its block.
    nodes_[lh.next].prev = leaf;
    lf.head = nodes_[lh.next].head;
    lh.next = leaf;
    lf.prev = lhead_;
    lf.left = lf.right = kNone;

    if (lh.parent != kNone) {
        if (nodes_[lh.parent].left == lhead_) nodes_[lh.parent].left = inner;
        else nodes_[lh.parent].right = inner;
    } else {
        tree_ = inner;
    }
    in.right = leaf;
    in.left = lhead_;
    in.parent = lh.parent;
    lh.parent = inner;
    lf.parent = inner;
    loc_[symbol] = leaf;
    increment(in.parent);
}

std::string Huffman::code(int symbol) const {
    std::string path;
    for (int n = loc_[symbol]; n != kNone && nodes_[n].parent != kNone; n = nodes_[n].parent)
        path.insert(path.begin(), nodes_[nodes_[n].parent].right == n ? '1' : '0');
    return path;
}

int Huffman::compress(const uint8_t* in, int len, uint8_t* out, int outCapacity) const {
    int bit = 0;
    char path[64];
    for (int i = 0; i < len; ++i) {
        int depth = 0;
        for (int n = loc_[in[i]]; nodes_[n].parent != kNone; n = nodes_[n].parent)
            path[depth++] = nodes_[nodes_[n].parent].right == n ? 1 : 0;
        while (depth > 0) {
            int byte = bit >> 3;
            if (byte >= outCapacity) return outCapacity;
            if ((bit & 7) == 0) out[byte] = 0;
            out[byte] |= static_cast<uint8_t>(path[--depth] << (bit & 7));
            ++bit;
        }
    }
    return (bit + 7) >> 3;
}

int Huffman::decompress(const uint8_t* in, int len, uint8_t* out, int outCapacity) const {
    const int total = len * 8;
    int bit = 0;
    int count = 0;
    while (bit < total && count < outCapacity) {
        int n = tree_;
        while (nodes_[n].symbol == kInternal) {
            int b = bit < total ? (in[bit >> 3] >> (bit & 7)) & 1 : 0;
            ++bit;
            n = b ? nodes_[n].right : nodes_[n].left;
        }
        out[count++] = static_cast<uint8_t>(nodes_[n].symbol);
    }
    return count;
}

} // namespace iw2::net
