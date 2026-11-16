// lookup_result.h
#pragma once
#include <string>

enum class LookupState
{
    kNotFound,
    kValue,
    kDeleted
};

struct LookupResult
{
    LookupState state = LookupState::kNotFound;
    std::string value;
};