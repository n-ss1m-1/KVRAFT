// manifest.h
#pragma once
#include <cstdint>
#include <vector>
#include <string>


#include "internal_key.h"
#include "coding.h"
#include "noncopyable.h"


struct TableMeta
{
    uint64_t fileNumber = 0;        // SSTable编号(从1开始)
    uint32_t level = 0;             // 为compactoion预留
    uint64_t fileSize = 0;          // 文件大小 启动时快速校验
    uint64_t minSequence = 0;       
    uint64_t maxSequence = 0;       // 为compactoion预留
    InternalKey smallest;       
    InternalKey largest;            // 查找时快速判断是否存在某个key
};


class Manifest  //! Manifest 不持有文件描述符，只包含整数和 vector，属于普通值对象 -> 无需集成Noncopyable -> Load返回时移动构造更自然
{
public:
    // 用于创建新的manifest / 更新manifest
    Manifest() = default;

    // 用于加载已有的manifest(与构造函数区别)
    // 打开文件 + 获取文件大小 -> 循环读取 -> decode -> 返回manifest
    static Manifest Load(const std::string& path);

    // EncodeManifest -> 循环写到path.tmp上 -> fsync + close 临时文件 -> rename原子替换 -> fsync 父目录
    // 会同步父目录
    void Save(const std::string& path) const;       // ! 不自动递增generation

    void AddTable(TableMeta meta);
    void RemoveTable(uint64_t fileNumber);

    void SetLastSequence(uint64_t lastSeuquence) noexcept;
    void SetFlushedSequence(uint64_t flushedSequence) noexcept;
    void SetGeneration(uint64_t generation) noexcept;

    uint64_t LastSequence() const noexcept;
    uint64_t FlushedSequence() const noexcept;
    uint64_t Generation() const noexcept;
    const std::vector<TableMeta>& Tables() const noexcept;      // 只返回可读引用 而非副本

private:
    uint64_t lastSequence_ = 0;         // db最后见到的sequence
    uint64_t flushedSequence_ = 0;      // 刷入sstable的最大sequence
    uint64_t generation_ = 0;           // 多个MANIFEST文件时的版本序号
    std::vector<TableMeta> tables_;
};