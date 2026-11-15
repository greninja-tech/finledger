#include "lsm_store.hpp"
#include "mvcc_store.hpp"
#include "raft_simulator.hpp"
#include "shard_router.hpp"
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc,char** argv){
    const std::string command=argc>1?argv[1]:"demo";
    try{
        if(command=="lsm"){
            const auto dir=argc>2?std::filesystem::path(argv[2]):std::filesystem::path("systems_lab_data");
            LsmStore store(dir,3);store.put("account:1001","50000");store.put("account:1002","25000");store.put("account:1001","47500");store.erase("account:1002");store.flush();
            std::string value;std::cout<<"LSM demo: SSTables="<<store.sstable_count()<<", flushes="<<store.flush_count()<<", WAL records="<<store.wal_record_count()<<"\n";
            std::cout<<"account:1001="<<(store.get("account:1001",value)?value:"<missing>")<<"\n";
            std::cout<<"account:1002="<<(store.get("account:1002",value)?value:"<missing>")<<"\n";
            store.compact();std::cout<<"After compaction: SSTables="<<store.sstable_count()<<", compactions="<<store.compaction_count()<<"\n";return 0;
        }
        if(command=="raft"){
            RaftSimulator raft({"node-1","node-2","node-3"});if(!raft.elect_leader())throw std::runtime_error("no majority for election");
            std::cout<<"Leader elected: "<<*raft.leader_id()<<"\n";raft.append("TXN-001 committed");
            auto old=*raft.leader_id();raft.set_online(old,false);std::cout<<"Leader "<<old<<" failed. New election: "<<(raft.elect_leader()?*raft.leader_id():"NO MAJORITY")<<"\n";
            std::cout<<"Committed entries on online nodes: "<<raft.committed_entries()<<" (in-process simulator)\n";return 0;
        }
        if(command=="mvcc"){
            MvccStore db;db.put("ACC-1","10000");auto snapshot=db.snapshot();db.put("ACC-1","7000");
            std::cout<<"MVCC old snapshot="<<*db.get("ACC-1",snapshot)<<", latest="<<*db.get("ACC-1",db.snapshot())<<"\n";return 0;
        }
        if(command=="shard"){
            ShardRouter router({"SHARD-01","SHARD-02","SHARD-03"});for(const auto& key:{"ACC-001","ACC-002","ACC-003","ACC-004"})std::cout<<key<<" -> "<<router.route(key)<<"\n";return 0;
        }
        std::cout<<"FinLedger Systems Lab\nCommands: lsm [data-dir], mvcc, shard, raft\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"Systems lab error: "<<e.what()<<"\n";return 1;}
}
