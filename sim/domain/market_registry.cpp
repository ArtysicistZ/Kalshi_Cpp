#include "sim/domain/market_registry.h"

namespace kalshi::sim {

bool MarketRegistry::add_market(std::string ticker) {
    auto [it, inserted] = engines_.try_emplace(
        std::move(ticker),
        std::make_unique<MatchingEngine>()
    );
    return inserted;
}

MatchingEngine* MarketRegistry::get_engine(const std::string& ticker) {
    auto it = engines_.find(ticker);
    if (it == engines_.end()) return nullptr;
    return it->second.get();
}

bool MarketRegistry::has_market(const std::string& ticker) const {
    return engines_.find(ticker) != engines_.end();
}

std::vector<std::string> MarketRegistry::list_markets() const {
    std::vector<std::string> markets;
    markets.reserve(engines_.size());
    for (const auto& [ticker, _] : engines_) {
        markets.push_back(ticker);
    }
    return markets;
}

}