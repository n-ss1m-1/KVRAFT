#pragma once
#include "lsm/coding.h"
namespace waltest {
inline std::string Frame(const lsm::Entry& entry) {
    const auto payload = lsm::EncodeEntry(entry);
    std::string bytes;
    lsm::AppendFixed32(bytes, static_cast<uint32_t>(payload.size()));
    lsm::AppendFixed32(bytes, lsm::Crc32c(bytes + payload));
    bytes += payload;
    return bytes;
}
}
