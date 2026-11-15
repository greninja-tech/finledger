#include "lsm_store.hpp"
#include "mvcc_store.hpp"
#include "raft_simulator.hpp"
#include "shard_router.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
int main(){
  auto dir=std::filesystem::temp_directory_path()/"finledger_lsm_test";std::filesystem::remove_all(dir);
  {LsmStore db(dir,2);db.put("a","one");db.put("b","two");db.flush();db.put("a","three");db.erase("b");db.flush();std::string v;assert(db.get("a",v)&&v=="three");assert(!db.get("b",v));assert(db.sstable_count()==2);db.compact();assert(db.sstable_count()==1);}
  {LsmStore db(dir,2);std::string v;assert(db.get("a",v)&&v=="three");assert(!db.get("b",v));db.put("wal-only","recovered");}
  {LsmStore db(dir,10);std::string v;assert(db.get("wal-only",v)&&v=="recovered");}
  MvccStore mv;mv.put("x","old");auto snap=mv.snapshot();mv.put("x","new");assert(*mv.get("x",snap)=="old");assert(*mv.get("x",mv.snapshot())=="new");
  ShardRouter router({"s1","s2","s3"});assert(!router.route("x").empty());
  RaftSimulator raft({"n1","n2","n3"});assert(raft.elect_leader());assert(raft.append("entry-1"));auto leader=*raft.leader_id();raft.set_online(leader,false);assert(raft.elect_leader());assert(raft.committed_entries()==1);auto next=*raft.leader_id();for(const auto& n:raft.nodes())if(n.id!=next)raft.set_online(n.id,false);assert(!raft.append("must-not-commit"));
  std::filesystem::remove_all(dir);std::cout<<"PASS: LSM WAL/SSTable/tombstone/compaction/recovery, MVCC snapshots, logical sharding, Raft majority/failover/no-quorum simulation\n";
}
