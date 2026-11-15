#pragma once
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

// Deterministic logical partitioning. This routes keys; it does not replicate data.
class ShardRouter {
    std::vector<std::string> shards_;
public:
    explicit ShardRouter(std::vector<std::string> shards):shards_(std::move(shards)){if(shards_.empty())throw std::invalid_argument("at least one shard is required");}
    const std::string& route(const std::string& key)const{return shards_[std::hash<std::string>{}(key)%shards_.size()];}
    std::size_t shard_count()const{return shards_.size();}
};
