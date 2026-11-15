#pragma once
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Deterministic in-process Raft teaching simulator. Nodes are objects in one
// process; this is not a networked consensus implementation.
class RaftSimulator {
public:
    struct Node { std::string id; bool online{true}; int term{0}; std::vector<std::string> log; std::size_t commit_index{0}; bool leader{false}; };
private:
    std::vector<Node> nodes_;
    std::optional<std::size_t> leader_;
    int term_{0};
    std::size_t majority()const{return nodes_.size()/2+1;}
public:
    explicit RaftSimulator(std::vector<std::string> ids) {
        if (ids.size() < 3 || ids.size() % 2 == 0)
            throw std::invalid_argument("use an odd cluster size of at least three");
        for (auto& id : ids) nodes_.push_back(Node{id, true, 0, {}, 0, false});
    }
    bool elect_leader() {
        for (auto& node : nodes_) node.leader = false;
        leader_.reset();
        std::size_t online = 0;
        for (const auto& node : nodes_) if (node.online) ++online;
        if (online < majority()) return false;
        std::size_t candidate = nodes_.size();
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].online && (candidate == nodes_.size() || nodes_[i].log.size() > nodes_[candidate].log.size())) candidate = i;
        }
        ++term_;
        nodes_[candidate].term = term_;
        nodes_[candidate].leader = true;
        leader_ = candidate;
        return true;
    }
    bool append(const std::string& command){
        if(!leader_||!nodes_[*leader_].online)throw std::logic_error("no active leader; elect a leader first");
        std::size_t acknowledgements=0;for(const auto& n:nodes_)if(n.online)++acknowledgements;
        if(acknowledgements<majority())return false;
        const auto entry=command;for(auto& n:nodes_)if(n.online){if(n.log.size()>nodes_[*leader_].log.size())n.log.resize(nodes_[*leader_].log.size());n.log.push_back(entry);n.term=term_;}
        for (auto& node : nodes_) if (node.online) node.commit_index = nodes_[*leader_].log.size();
        return true;
    }
    void set_online(const std::string& id,bool online){for(auto& n:nodes_)if(n.id==id){n.online=online;if(!online&&leader_&&nodes_[*leader_].id==id){nodes_[*leader_].leader=false;leader_.reset();}return;}throw std::out_of_range("unknown node");}
    std::optional<std::string> leader_id()const{return leader_?std::optional<std::string>(nodes_[*leader_].id):std::nullopt;}
    const std::vector<Node>& nodes()const{return nodes_;}
    std::size_t committed_entries()const{for(const auto& n:nodes_)if(n.online)return n.commit_index;return 0;}
    int current_term()const{return term_;}
};
