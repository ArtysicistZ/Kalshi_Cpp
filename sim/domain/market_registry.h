#pragma once

#include "sim/domain/matching_engine.h"

#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

namespace kalshi::sim {

class MarketRegistry {
private:
    std::unordered_map<std::string, std::unique_ptr<MatchingEngine>> engines_;

public:
    MarketRegistry() = default;
    ~MarketRegistry() = default;

    MarketRegistry(const MarketRegistry&) = delete;
    MarketRegistry& operator=(const MarketRegistry&) = delete;

    bool add_market(std::string ticker);
    MatchingEngine* get_engine(const std::string& ticker);
    bool has_market(const std::string& ticker) const;
    std::vector<std::string> list_markets() const;

};

}