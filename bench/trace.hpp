#pragma once

// Binary operation trace: the exact input stream a benchmark replays.
//
// Layout (little-endian, which both Apple Silicon and x86-64 are):
//   header : 8-byte magic "OBTRACE1", u64 record count
//   record : 32 bytes each
//            u8 kind (0 submit, 1 cancel, 2 modify), u8 side, u8 type,
//            5 pad bytes, u64 id, i64 price, i64 qty
// Fixed-size records mean the Python exporter can write it with one
// struct.pack format ("<BBB5xQqq") and C++ can read it with no parsing logic.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "ob/op.hpp"

namespace obbench {

using namespace ob;

inline constexpr std::array<char, 8> kTraceMagic{'O', 'B', 'T', 'R', 'A', 'C', 'E', '1'};

struct TraceRecord {
    std::uint8_t kind;
    std::uint8_t side;
    std::uint8_t type;
    std::uint8_t pad[5];
    std::uint64_t id;
    std::int64_t price;
    std::int64_t qty;
};
static_assert(sizeof(TraceRecord) == 32, "trace records must match the Python struct format");

inline std::vector<Op> read_trace(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open trace " + path);
    std::array<char, 8> magic{};
    std::uint64_t count = 0;
    if (std::fread(magic.data(), 1, 8, f) != 8 || magic != kTraceMagic ||
        std::fread(&count, sizeof count, 1, f) != 1) {
        std::fclose(f);
        throw std::runtime_error("bad trace header in " + path);
    }
    std::vector<Op> ops;
    ops.reserve(count);
    TraceRecord r{};
    for (std::uint64_t i = 0; i < count; ++i) {
        if (std::fread(&r, sizeof r, 1, f) != 1) {
            std::fclose(f);
            throw std::runtime_error("truncated trace " + path);
        }
        Op op;
        op.kind = static_cast<OpKind>(r.kind);
        op.order = NewOrder{r.id, static_cast<Side>(r.side), static_cast<OrderType>(r.type), r.price, r.qty};
        ops.push_back(op);
    }
    std::fclose(f);
    return ops;
}

inline void write_trace(const std::string& path, const std::vector<Op>& ops) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write trace " + path);
    const std::uint64_t count = ops.size();
    std::fwrite(kTraceMagic.data(), 1, 8, f);
    std::fwrite(&count, sizeof count, 1, f);
    for (const Op& op : ops) {
        TraceRecord r{};
        r.kind = static_cast<std::uint8_t>(op.kind);
        r.side = static_cast<std::uint8_t>(op.order.side);
        r.type = static_cast<std::uint8_t>(op.order.type);
        r.id = op.order.id;
        r.price = op.order.price;
        r.qty = op.order.qty;
        std::fwrite(&r, sizeof r, 1, f);
    }
    std::fclose(f);
}

}  // namespace obbench
