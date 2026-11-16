// manifest.cc
#include <unistd.h>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <cstddef>      // std::byte
#include <utility>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <stdexcept>
#include <system_error>

#include "manifest.h"

namespace 
{
    constexpr std::string_view kManifestMagic = "MYMAN001";     // 这里的命名和放置的位置合适吗? 是否需要添加其他的常量?
    constexpr uint32_t kManifestVersion = 1;
    constexpr uint32_t kManifestHeaderSize = 48;            // 仅表示Header的字节数，不包含TableMeta和CRC
    constexpr uint32_t kManifestChecksumSize = 4;

    constexpr uint32_t kMinTableMetaSize = 62;

    // MANIFEST文件总大小 = headerSize + payloadSize + checksumSize
    /* 二进制布局：
    ManifestHeader
    magic             8 bytes   "MYMAN001"  标识这是 MANIFEST 文件
    version           4 bytes   决定如何解析文件布局
    headerSize        4 bytes   不包含 payload 和 CRC
    payloadSize       4 bytes   所有 TableMeta 编码拼接后的长度 不包含CRC
    generation        8 bytes   Manifest 快照发布编号
    lastSequence      8 bytes
    flushedSequence   8 bytes
    tableCount        4 bytes

    TableMeta × tableCount
        fileNumber        8 bytes
        level             4 bytes
        fileSize          8 bytes
        minSequence       8 bytes
        maxSequence       8 bytes
        smallestKey       variable bytes
        largestKey        variable bytes

    CRC32C                4 bytes

    注: InternalKey:[sequence:8][type:1][key_len:4][user_key]
    */

    /*
    TableMeta待检查的条件：
     - fileNumber > 0
     - fileSize > 0
     - minSequence > 0
     - minSequence <= maxSequence (!单条记录时取等)
     - smallest <= largest
     - 两个边界键的 sequence 均在 [minSequence, maxSequence]

    在EncodeTableMeta和DecodeTableMeta中共同使用 (false -> 抛异常/返回false)
    */
    bool IsValidTableMeta(const TableMeta& tableMeta)
    {
        if(tableMeta.fileNumber <= 0) return false;
        if(tableMeta.fileSize <= 0) return false;
        if(tableMeta.minSequence <= 0) return false;
        if(tableMeta.minSequence > tableMeta.maxSequence) return false;
        InternalKeyComparator cmp;
        if(cmp.Compare(tableMeta.smallest, tableMeta.largest) > 0) return false;
        if(tableMeta.smallest.sequence < tableMeta.minSequence || tableMeta.maxSequence < tableMeta.smallest.sequence 
            || tableMeta.largest.sequence < tableMeta.minSequence || tableMeta.maxSequence < tableMeta.largest.sequence)    return false;

        return true;
    }
    size_t EncodedTableMetaSize(const TableMeta& tableMeta)
    {
        // 检查 36 + 两个InternalKey 长度是否溢出
        if(lsm::EncodedInternalKeySize(tableMeta.smallest) > std::numeric_limits<size_t>::max() - 36 
            || lsm::EncodedInternalKeySize(tableMeta.largest) > std::numeric_limits<size_t>::max() - 36 - lsm::EncodedInternalKeySize(tableMeta.smallest)) 
            throw std::invalid_argument("EncodedTableMetaSize: key length overflow");

        return kMinTableMetaSize + tableMeta.smallest.user_key.size() + tableMeta.largest.user_key.size();
    }
    std::string EncodeTableMeta(const TableMeta& tableMeta)
    {
        // TableMeta合法性检查
        if(!IsValidTableMeta(tableMeta)) throw std::invalid_argument("EncodeTableMeta: invalid TableMeta");

        std::string encoded;
        encoded.reserve(EncodedTableMetaSize(tableMeta));

        lsm::AppendFixed64(encoded, tableMeta.fileNumber);
        lsm::AppendFixed32(encoded, tableMeta.level);
        lsm::AppendFixed64(encoded, tableMeta.fileSize);
        lsm::AppendFixed64(encoded, tableMeta.minSequence);
        lsm::AppendFixed64(encoded, tableMeta.maxSequence);

        encoded.append(lsm::EncodeInternalKey(tableMeta.smallest));
        encoded.append(lsm::EncodeInternalKey(tableMeta.largest));

        return encoded;
    }

    bool DecodeTableMeta(std::string_view input, TableMeta& tableMeta, size_t& bytesRead)   // !成功后通过 bytesRead 告诉外层前进多少字节
    {
        if(input.size() < kMinTableMetaSize) return false;

        TableMeta decoded;
        size_t offset = 0;        // ! 使用offset来递增偏移量 -> 更直观

        decoded.fileNumber = lsm::DecodeFixed64(input.data() + offset);
        offset += sizeof(decoded.fileNumber);
        decoded.level = lsm::DecodeFixed32(input.data() + offset);
        offset += sizeof(decoded.level);
        decoded.fileSize = lsm::DecodeFixed64(input.data() + offset);
        offset += sizeof(decoded.fileSize);
        decoded.minSequence = lsm::DecodeFixed64(input.data() + offset);
        offset += sizeof(decoded.minSequence);
        decoded.maxSequence = lsm::DecodeFixed64(input.data() + offset);
        offset += sizeof(decoded.maxSequence);
        
        // ! 已封装InternalKey的encode和decode     
        // ! 注意：下面不能传input.data()+offset 这样会隐式转换为string_view 通过寻找'\0'确定长度 二进制数据不能这样处理 -> 通过string_view.substr(offset) 显示截取就不会有这样的问题了
        size_t keyBytes = 0;
        if(!lsm::DecodeInternalKey(input.substr(offset), decoded.smallest, keyBytes)) return false;
        offset += keyBytes;
        if(!lsm::DecodeInternalKey(input.substr(offset), decoded.largest, keyBytes)) return false;
        offset += keyBytes;

        // TableMeta合法性检查(!先检查 再提交)
        if(!IsValidTableMeta(decoded)) return false;

        tableMeta = std::move(decoded);
        bytesRead = offset;

        return true;
    }

    /* 
    Manifest通用合法性检查: 
        - flushedSequence <= lastSequence
        - 各 TableMeta 合法(EncodeTableMeta/DecodeTableMeta时检查)
        - fileNumber 不重复 (依据unordered_set::insert()的返回值检查)
        - 每张表 maxSequence <= flushedSequence

    解码循环前，还需要检查：tableCount的合法性

    EncodeManifest()再增加数据范围检查：
        - 累加 payload 时，保证结果不超过 UINT32_MAX
        - reserve() 前检查 payloadSize + 52 不超过 size_t 范围
    */
    std::string EncodeManifest(const Manifest& manifest)
    {
        if(manifest.FlushedSequence() > manifest.LastSequence()) throw std::logic_error("EncodeManifest: flushedSequence > lastSequence");

        std::string encoded;
        size_t payloadSize = 0;
        for(const auto& meta : manifest.Tables()) 
        {
            size_t metaSize = EncodedTableMetaSize(meta);
            if(metaSize > UINT32_MAX - payloadSize) throw std::length_error("EncodeManifest: Manifest payload too large");
            payloadSize += metaSize;
        }
        if(payloadSize > std::numeric_limits<size_t>::max() - kManifestHeaderSize - kManifestChecksumSize) throw std::length_error("EncodeManifest: Manifest payload too large");
        encoded.reserve(kManifestHeaderSize + payloadSize + kManifestChecksumSize);

        encoded.append(kManifestMagic);
        lsm::AppendFixed32(encoded, kManifestVersion);
        lsm::AppendFixed32(encoded, kManifestHeaderSize);                   // !不包含 payload 和 CRC
        lsm::AppendFixed32(encoded, static_cast<uint32_t>(payloadSize));    // !payloadSize: 所有 TableMeta 编码拼接后的长度 不包含CRC
        lsm::AppendFixed64(encoded, manifest.Generation());
        lsm::AppendFixed64(encoded, manifest.LastSequence());
        lsm::AppendFixed64(encoded, manifest.FlushedSequence());
        lsm::AppendFixed32(encoded, static_cast<uint32_t>(manifest.Tables().size()));

        std::unordered_set<uint64_t> fileNumbers;
        for(const auto& meta : manifest.Tables()) 
        {
            if(meta.maxSequence > manifest.FlushedSequence()) throw std::logic_error("EncodeManifest: table meta maxSequence exceeds flushedSequence");
            auto [_,Inserted] = fileNumbers.insert(meta.fileNumber);     // true: 之前不存在，插入成功 | false: 之前存在，插入失败
            if(!Inserted) throw std::logic_error("EncodeManifest: duplicate fileNumber in manifest tables");

            encoded.append(EncodeTableMeta(meta));
        }

        lsm::AppendFixed32(encoded, lsm::Crc32c(encoded));
    
        return encoded;
    }

    bool DecodeManifest(std::string_view input, Manifest& manifest)
    {
        // 校验文件大小(至少52字节) + magic + version + headerSize
        // 不同的version使用不同的解析方式
        if(input.size() < kManifestHeaderSize + kManifestChecksumSize) return false;

        size_t offset = 0;
        if(kManifestMagic != input.substr(offset,kManifestMagic.size())) return false;
        offset += kManifestMagic.size();
        if(kManifestVersion != lsm::DecodeFixed32(input.data() + offset)) return false;
        offset += sizeof(kManifestVersion);
        if(kManifestHeaderSize != lsm::DecodeFixed32(input.data() + offset)) return false;
        offset += sizeof(kManifestHeaderSize);

        // 校验payloadSize
        uint32_t payloadSize = lsm::DecodeFixed32(input.data() + offset);
        offset += sizeof(payloadSize);
        if(payloadSize != input.size() - kManifestHeaderSize - kManifestChecksumSize) return false;

        // 校验CRC
        uint32_t rawCrc = lsm::DecodeFixed32(input.data() + kManifestHeaderSize + payloadSize);
        if(rawCrc != lsm::Crc32c(input.substr(0, kManifestHeaderSize + payloadSize))) return false;
        
        // 读取其余 Header 字段 
        Manifest decoded;
        decoded.SetGeneration(lsm::DecodeFixed64(input.data() + offset));
        offset += 8;
        decoded.SetLastSequence(lsm::DecodeFixed64(input.data() + offset));
        offset += 8;
        decoded.SetFlushedSequence(lsm::DecodeFixed64(input.data() + offset));
        offset += 8;
        if(decoded.FlushedSequence() > decoded.LastSequence()) return false;        // 检查
        uint32_t tableCount = lsm::DecodeFixed32(input.data() + offset);
        offset += sizeof(tableCount);

        // 解析 tableCount 条 TableMeta
        if(tableCount > payloadSize / kMinTableMetaSize) return false;
        std::unordered_set<uint64_t> fileNumbers;
        const auto payload = input.substr(kManifestHeaderSize, payloadSize);    // ! 显式截取payload 而非每次使用剩余部分(input.substr(offset))
        size_t payloadOffset = 0;
        for(uint32_t i=0; i<tableCount; i++) 
        {
            TableMeta meta;
            size_t bytesRead = 0;
            // 解码
            if(!DecodeTableMeta(payload.substr(payloadOffset), meta, bytesRead)) return false;
            
            // 检查
            if(meta.maxSequence > decoded.FlushedSequence()) return false;
            auto [_,notInserted] = fileNumbers.insert(meta.fileNumber);     // true: 之前不存在，插入成功 | false: 之前存在，插入失败
            if(!notInserted) return false;

            // 提交
            payloadOffset += bytesRead;
            decoded.AddTable(std::move(meta));          // move更快捷
        }

        // payload 恰好消费完毕
        if(payloadOffset != payload.size()) return false;
        
        manifest = std::move(decoded);

        return true;
    }



    // ! 由外部分配地址空间: char*->malloc  string->resize()
    void ReadAll(int fd, void* buf, size_t size)
    {
        std::byte* output = static_cast<std::byte*>(buf);

        size_t done = 0;
        while(done < size)
        {
            ssize_t n = read(fd, output+done, size-done);
            if(n == 0) throw std::runtime_error("ReadAll: unexpected EOF");         // ! 提前遇到未知EOF 应该报错
            if(n < 0)
            {
                if(errno == EINTR) continue;
                throw std::system_error(errno,std::generic_category(),"ReadAll: read system call failed");
            }
            done += static_cast<size_t>(n);
        }
    }
    void WriteAll(int fd, const void* buf, size_t size)                 // const: 只读取缓冲区
    {
        const std::byte* intput = static_cast<const std::byte*>(buf);

        size_t done = 0;
        while(done < size)
        {
            ssize_t n = write(fd, intput+done, size-done);
            if(n == 0) throw std::system_error(std::make_error_code(std::errc::io_error), "WriteAll: no progress");
            if(n < 0)
            {
                if(errno == EINTR) continue;
                throw std::system_error(errno,std::generic_category(),"WriteAll: write system call failed");
            }
            done += static_cast<size_t>(n);
        }
    }

}


// 打开文件 + 获取文件大小 -> 循环读取 -> decode -> 返回manifest
Manifest Manifest::Load(const std::string& path) 
{
    // 打开文件
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0) throw std::system_error(errno, std::generic_category(), "Manifest::Load: open manifest file failed");
    
    Manifest manifest;
    try
    {
        // 获取文件大小
        struct stat st{};
        if(fstat(fd, &st) < 0) throw std::system_error(errno, std::generic_category(), "Manifest::Load: fstat failed");
        if(st.st_size < kManifestHeaderSize + kManifestChecksumSize || uint64_t(UINT32_MAX) + kManifestHeaderSize + kManifestChecksumSize < st.st_size) throw std::runtime_error("Manifest::Load: manifest file size invalid, corrupted");
        size_t fileSize = static_cast<size_t>(st.st_size);

        // 循环读取
        std::string encoded;
        encoded.resize(fileSize);                       //! 必须分配内存空间 否则无法写入
        ReadAll(fd, encoded.data(), fileSize);

        int closeResult = close(fd);        // !正常路径也需要关闭fd
        int closeError = errno;
        fd = -1;
        if(closeResult < 0) throw std::system_error(closeError, std::generic_category(), "Manifest::Load: close manifest file failed");

        // 解析Manifest
        if(!DecodeManifest(encoded, manifest)) throw std::runtime_error("Manifest::Load: decode manifest failed, file corrupted");
    }
    catch(...)
    {
        if(fd >= 0) close(fd);      // ! 不重复close同一fd
        throw;
    }

    return manifest;
}

// EncodeManifest -> 循环写到path.tmp上 -> fsync + close 临时文件 -> rename原子替换 -> fsync 父目录
void Manifest::Save(const std::string& path) const 
{
    // 小端二进制编码Manifest
    std::string encoded = EncodeManifest(*this);

    // 打开tmp临时文件
    std::string tmpPath = path + ".tmp";
    int tmpFd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(tmpFd < 0) throw std::system_error(errno, std::generic_category(), "Manifest::Save: open tmp file failed");
    
    try
    {
        // 将Manifest写入tmp + 刷盘 + 关闭tmp文件 + 重命名为Manifest
        WriteAll(tmpFd, encoded.data(), encoded.size());

        while(fsync(tmpFd) < 0)
        {
            if(errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "Manifest::Save: fsync tmp file failed");
        }
        
        int closeResult = close(tmpFd);
        int closeError = errno;
        tmpFd = -1;
        if(closeResult < 0) throw std::system_error(closeError, std::generic_category(), "Manifest::Save: close tmp file failed");

        if(rename(tmpPath.c_str(),path.c_str()) < 0) throw std::system_error(errno, std::generic_category(), "Manifest::Save: atomic rename failed");
    }
    catch(...)
    {
        // 任意系统调用异常：关闭fd + 清理tmp文件
        if(tmpFd >= 0) close(tmpFd);                // ! 不重复close同一fd

        unlink(tmpPath.c_str());       // 删除tmp文件
        throw;                              // 继续向外抛出原来的异常
    }

    // ! fsync父目录
    std::filesystem::path parentDir = std::filesystem::path(path).parent_path();
    if(parentDir.empty()) parentDir = ".";

    int dirFd = open(parentDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);              // ! 目录是特殊文件，只能以只读方式打开  |  fsync()也不需要写权限
    if(dirFd < 0) throw std::system_error(errno, std::generic_category(), "Manifest::Save: open parent dir failed");
    while(fsync(dirFd) < 0)
    {
        if(errno == EINTR) continue;
        int err = errno;
        close(dirFd);
        throw std::system_error(err, std::generic_category(), "Manifest::Save: fsync parent dir failed");
    }
    
    if(close(dirFd) < 0) throw std::system_error(errno, std::generic_category(), "Manifest::Save: close parent dir failed");
}

void Manifest::AddTable(TableMeta meta) 
{
    tables_.push_back(std::move(meta));
}

void Manifest::RemoveTable(uint64_t fileNumber) 
{
    for(auto it = tables_.begin(); it!=tables_.end(); it++)
    {
        if(it->fileNumber == fileNumber)
        {
            tables_.erase(it);
            break;
        }
    }
}

void Manifest::SetLastSequence(uint64_t lastSeuquence) noexcept 
{
    lastSequence_ = lastSeuquence;
}

void Manifest::SetFlushedSequence(uint64_t flushedSequence) noexcept 
{
    flushedSequence_ = flushedSequence;
} 

void Manifest::SetGeneration(uint64_t generation) noexcept 
{
    generation_ = generation;
}

uint64_t Manifest::LastSequence() const noexcept 
{
    return lastSequence_;
}

uint64_t Manifest::FlushedSequence() const noexcept 
{
    return flushedSequence_;
}

uint64_t Manifest::Generation() const noexcept 
{
    return generation_;
}

const std::vector<TableMeta>& Manifest::Tables() const noexcept 
{
    return tables_;
}


