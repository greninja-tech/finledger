#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Small MVCC teaching model. Readers pin a revision; writers append versions.
class MvccStore {
    struct Version { std::uint64_t revision; std::string value; bool deleted; };
    std::map<std::string,std::vector<Version>> data_;
    std::uint64_t revision_{0};
public:
    std::uint64_t put(const std::string& key,const std::string& value){auto r=++revision_;data_[key].push_back({r,value,false});return r;}
    std::uint64_t erase(const std::string& key){auto r=++revision_;data_[key].push_back({r,{},true});return r;}
    std::uint64_t snapshot()const{return revision_;}
    std::optional<std::string> get(const std::string& key,std::uint64_t at)const{
        auto it=data_.find(key);if(it==data_.end())return std::nullopt;
        for(auto v=it->second.rbegin();v!=it->second.rend();++v)if(v->revision<=at)return v->deleted?std::nullopt:std::optional<std::string>(v->value);
        return std::nullopt;
    }
    std::uint64_t current_revision()const{return revision_;}
};
