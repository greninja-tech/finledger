#pragma once
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Educational single-process LSM key/value store: append-only WAL, sorted
// immutable SSTables, an in-memory sparse index, Bloom filter and compaction.
// It is intentionally not a production database (no fsync/checksum/locking).
class LsmStore {
    struct Table {
        std::filesystem::path path;
        std::unordered_map<std::string, std::streamoff> index;
        std::vector<std::uint64_t> bloom;
        std::size_t bloom_bits{8192};
        bool might_contain(const std::string& key) const {
            if (bloom.empty()) return false;
            const auto h1 = std::hash<std::string>{}(key);
            const auto h2 = std::hash<std::string>{}(std::string("#") + key) | 1ULL;
            for (std::uint64_t i=0;i<4;++i) {
                const auto bit=(h1+i*h2)%bloom_bits;
                if ((bloom[bit/64] & (1ULL<<(bit%64)))==0) return false;
            }
            return true;
        }
        void add_bloom(const std::string& key) {
            if (bloom.empty()) bloom.resize((bloom_bits+63)/64);
            const auto h1=std::hash<std::string>{}(key);
            const auto h2=std::hash<std::string>{}(std::string("#")+key)|1ULL;
            for(std::uint64_t i=0;i<4;++i){const auto bit=(h1+i*h2)%bloom_bits;bloom[bit/64]|=(1ULL<<(bit%64));}
        }
    };
    using Record = std::pair<bool,std::string>; // true=present, false=tombstone
    std::filesystem::path dir_, wal_;
    std::map<std::string,Record> mem_;
    std::vector<Table> tables_; // oldest to newest
    std::size_t flush_threshold_;
    std::uint64_t flushes_{0}, compactions_{0}, wal_records_{0};

    static std::string hex(const std::string& s) {
        static const char* d="0123456789abcdef"; std::string out; out.reserve(s.size()*2);
        for(unsigned char c:s){out.push_back(d[c>>4]);out.push_back(d[c&15]);} return out;
    }
    static std::string unhex(const std::string& s) {
        if(s.size()%2) throw std::runtime_error("bad hex record");
        auto n=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
        std::string out; out.reserve(s.size()/2);
        for(std::size_t i=0;i<s.size();i+=2){int a=n(s[i]),b=n(s[i+1]);if(a<0||b<0)throw std::runtime_error("bad hex record");out.push_back(static_cast<char>((a<<4)|b));}return out;
    }
    static std::string encode(const std::string& key,const Record& r){return std::string(r.first?"P ":"D ")+hex(key)+" "+(r.first?hex(r.second):"-")+"\n";}
    static bool decode(const std::string& line,std::string& key,Record& r){std::istringstream in(line);char op;std::string k,v;if(!(in>>op>>k>>v)||(op!='P'&&op!='D'))return false;try{key=unhex(k);r={op=='P',op=='P'?unhex(v):std::string{}};return true;}catch(...){return false;}}
    void load_table(const std::filesystem::path& p){
        Table t; t.path=p; std::ifstream in(p,std::ios::binary); if(!in)throw std::runtime_error("cannot read SSTable: "+p.string());
        std::string line; while(in){auto pos=in.tellg();if(!std::getline(in,line))break;std::string k;Record r;if(!decode(line,k,r))throw std::runtime_error("corrupt SSTable record");t.index[k]=pos;t.add_bloom(k);}tables_.push_back(std::move(t));
    }
    Record read_table(const Table& t,const std::string& key) const {
        auto it=t.index.find(key);if(it==t.index.end())return {false,{}};
        std::ifstream in(t.path,std::ios::binary);in.seekg(it->second);std::string line;std::getline(in,line);std::string k;Record r;if(!decode(line,k,r)||k!=key)throw std::runtime_error("SSTable index mismatch");return r;
    }
    void append_wal(const std::string& key,const Record& r){std::ofstream out(wal_,std::ios::binary|std::ios::app);if(!out)throw std::runtime_error("cannot open WAL");out<<encode(key,r);out.flush();if(!out)throw std::runtime_error("WAL write failed");++wal_records_;}
    std::filesystem::path new_table_path(const std::string& suffix="") const {
        auto stamp=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        auto p=dir_/("table_"+std::to_string(stamp)+suffix+".sst");
        while(std::filesystem::exists(p)){++stamp;p=dir_/("table_"+std::to_string(stamp)+suffix+".sst");}
        return p;
    }
    void write_table(const std::map<std::string,Record>& data,const std::filesystem::path& path){
        auto tmp=path;tmp+=".tmp";std::ofstream out(tmp,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("cannot create SSTable");
        for (const auto& [k, v] : data) out << encode(k, v);
        out.flush();
        if (!out) throw std::runtime_error("SSTable write failed");
        out.close();
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(tmp, path, ec);
            if (ec) throw std::runtime_error("SSTable rename failed: " + ec.message());
        }
    }
public:
    explicit LsmStore(std::filesystem::path directory,std::size_t flush_threshold=64):dir_(std::move(directory)),wal_(dir_/"lsm.wal"),flush_threshold_(std::max<std::size_t>(1,flush_threshold)){
        std::filesystem::create_directories(dir_);
        std::vector<std::filesystem::path> files;for(const auto& e:std::filesystem::directory_iterator(dir_))if(e.path().extension()==".sst")files.push_back(e.path());
        std::sort(files.begin(),files.end());for(const auto& f:files)load_table(f);
        if(std::filesystem::exists(wal_)){std::ifstream in(wal_,std::ios::binary);std::string line;while(std::getline(in,line)){std::string k;Record r;if(decode(line,k,r)){mem_[k]=r;++wal_records_;}}}
        if(mem_.size()>=flush_threshold_)flush();
    }
    void put(const std::string& key,const std::string& value){if(key.empty())throw std::invalid_argument("key cannot be empty");Record r{true,value};append_wal(key,r);mem_[key]=r;if(mem_.size()>=flush_threshold_)flush();}
    void erase(const std::string& key){if(key.empty())throw std::invalid_argument("key cannot be empty");Record r{false,{}};append_wal(key,r);mem_[key]=r;if(mem_.size()>=flush_threshold_)flush();}
    bool get(const std::string& key,std::string& value) const {
        if(auto it=mem_.find(key);it!=mem_.end()){if(!it->second.first)return false;value=it->second.second;return true;}
        for(auto it=tables_.rbegin();it!=tables_.rend();++it){if(!it->might_contain(key))continue;auto r=read_table(*it,key);if(it->index.count(key)){if(!r.first)return false;value=r.second;return true;}}
        return false;
    }
    void flush(){if(mem_.empty())return;auto path=new_table_path();++flushes_;write_table(mem_,path);load_table(path);mem_.clear();std::ofstream out(wal_,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("cannot truncate WAL after flush");out.flush();if(!out)throw std::runtime_error("WAL truncation failed");wal_records_=0;}
    void compact(){
        if (tables_.size() < 2) return;
        std::map<std::string, Record> merged;
        for (const auto& table : tables_) {
            for (const auto& [key, _] : table.index) merged[key] = read_table(table, key);
        }
        for (const auto& [key, value] : mem_) merged[key] = value;
        for (auto it = merged.begin(); it != merged.end();) {
            if (!it->second.first) it = merged.erase(it);
            else ++it;
        }
        auto path = new_table_path("_compact");
        write_table(merged, path);
        for (const auto& table : tables_) std::filesystem::remove(table.path);
        tables_.clear();
        if (!merged.empty()) load_table(path);
        else std::filesystem::remove(path);
        mem_.clear();
        std::ofstream out(wal_, std::ios::binary | std::ios::trunc);
        out.flush();
        if (!out) throw std::runtime_error("cannot reset WAL after compaction");
        wal_records_ = 0;
        ++compactions_;
    }
    std::size_t memtable_size()const{return mem_.size();} std::size_t sstable_count()const{return tables_.size();}
    std::uint64_t flush_count()const{return flushes_;}std::uint64_t compaction_count()const{return compactions_;}std::uint64_t wal_record_count()const{return wal_records_;}
};
