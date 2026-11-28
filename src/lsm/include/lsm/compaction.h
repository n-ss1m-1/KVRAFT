// compaction.h
#pragma once
#include <vector>
#include <functional>

#include "coding.h"

class SSTableReader;


void MergeTables(const std::vector<const SSTableReader*>& tables, const std::function<void(const lsm::Entry&)>& emit);