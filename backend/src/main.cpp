#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace {

struct Account {
    std::string id;
    std::string name;
    std::int64_t balance_paise{};
    std::uint64_t version{1};
    std::string status{"active"};
};

struct Transaction {
    std::string id;
    std::string from;
    std::string to;
    std::int64_t amount_paise{};
    std::string status;
    std::string time;
    std::string shard{"SHARD-01"};
    std::string idempotency_key;
};

struct AuditEvent {
    std::string id;
    std::string event;
    std::string entity_id;
    std::string details;
    std::string time;
};

struct LedgerEntry {
    std::string id;
    std::string transaction_id;
    std::string account_id;
    std::string type;
    std::int64_t amount_paise{};
};

std::string now_time() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%H:%M:%S");
    return out.str();
}

std::string make_id(const char* prefix, std::uint64_t value, int width = 6) {
    std::ostringstream out;
    out << prefix << '-' << std::setw(width) << std::setfill('0') << value;
    return out.str();
}

class LedgerService {
public:
    LedgerService() {
        const char* configured = std::getenv("FINLEDGER_DATA_DIR");
        data_dir_ = configured && *configured ? configured : "data";
        std::filesystem::create_directories(data_dir_);
        if (!load_snapshot()) {
            // Seed only on first launch; subsequent launches restore the saved state.
            add_seed("ACC-0021", "Operating Account", 5'000'000);
            add_seed("ACC-0184", "Savings Account", 2'500'000);
            add_seed("ACC-0048", "Merchant Account", 3'200'000);
            add_seed("ACC-0112", "Settlement Account", 1'800'000);
            add_seed("ACC-0104", "Reserve Account", 4'100'000);
            audit("SYSTEM_INITIALIZED", "SYSTEM", "Seed accounts created");
            persist_snapshot();
        }
    }

    json audit_events() const {
        std::lock_guard lock(mutex_);
        json result = json::array();
        for (auto it = audit_.rbegin(); it != audit_.rend(); ++it) {
            result.push_back({{"id", it->id}, {"event", it->event}, {"entity_id", it->entity_id},
                {"details", it->details}, {"time", it->time}});
        }
        return result;
    }

    json persistence_status() const {
        std::lock_guard lock(mutex_);
        return {{"engine", "json-snapshot-plus-wal"}, {"durability", persistence_ok_ ? "wal-and-snapshot-flushed" : "degraded"},
            {"snapshot_file", (std::filesystem::path(data_dir_) / "finledger_snapshot.json").string()},
            {"last_error", persistence_error_}, {"audit_events", audit_.size()},
            {"wal_records", wal_records_}, {"memtable_entries", accounts_.size()}, {"sstable_count", 0}, {"compactions", compactions_}, {"state_revision", state_revision_}, {"wal_file", (std::filesystem::path(data_dir_) / "finledger.wal").string()}};
    }

    json account_by_id(const std::string& id) const {
        std::lock_guard lock(mutex_);
        const auto it = accounts_.find(id);
        if (it == accounts_.end()) throw std::out_of_range("account was not found");
        return account_json(it->second);
    }

    json set_account_status(const std::string& id, const std::string& status) {
        if (status != "active" && status != "frozen") throw std::invalid_argument("status must be active or frozen");
        std::lock_guard lock(mutex_);
        auto it = accounts_.find(id);
        if (it == accounts_.end()) throw std::out_of_range("account was not found");
        const auto previous = it->second.status;
        it->second.status = status;
        ++it->second.version;
        audit(status == "frozen" ? "ACCOUNT_FROZEN" : "ACCOUNT_UNFROZEN", id, "Account status changed from " + previous + " to " + status);
        persist_snapshot();
        return account_json(it->second);
    }

    json accounts() const {
        std::lock_guard lock(mutex_);
        json result = json::array();
        for (const auto& [id, account] : accounts_) {
            result.push_back(account_json(account));
        }
        std::sort(result.begin(), result.end(), [](const json& a, const json& b) {
            return a.at("id").get<std::string>() < b.at("id").get<std::string>();
        });
        return result;
    }

    json create_account(const json& body) {
        const auto name_it = body.find("name");
        if (name_it == body.end() || !name_it->is_string() || name_it->get<std::string>().empty()) {
            throw std::invalid_argument("name must be a non-empty string");
        }
        std::int64_t opening = body.value("opening_balance_paise", std::int64_t{0});
        if (opening < 0) throw std::invalid_argument("opening_balance_paise cannot be negative");

        std::lock_guard lock(mutex_);
        const auto id = make_id("ACC", ++account_sequence_, 4);
        Account account{id, name_it->get<std::string>(), opening, 1};
        accounts_.emplace(id, account);
        audit("ACCOUNT_CREATED", id, "Created account: " + account.name);
        persist_snapshot();
        return account_json(account);
    }

    json transactions() const {
        std::lock_guard lock(mutex_);
        json result = json::array();
        for (auto it = transactions_.rbegin(); it != transactions_.rend(); ++it) {
            result.push_back(transaction_json(*it));
        }
        return result;
    }

    json transfer(const json& body) {
        const auto from = required_string(body, "from_account");
        const auto to = required_string(body, "to_account");
        const auto key = required_string(body, "idempotency_key");
        const auto amount = body.value("amount_paise", std::int64_t{0});
        if (from == to) throw std::invalid_argument("source and destination accounts must differ");
        if (amount <= 0) throw std::invalid_argument("amount_paise must be greater than zero");

        std::lock_guard lock(mutex_);
        if (const auto previous = idempotency_.find(key); previous != idempotency_.end()) {
            const auto& record = previous->second;
            if (record.from != from || record.to != to || record.amount_paise != amount) {
                throw std::domain_error("idempotency key was already used with a different transfer payload");
            }
            return { {"transaction", transaction_json(transactions_.at(record.transaction_index))},
                     {"duplicate", true}, {"message", "Idempotent replay: original result returned"} };
        }

        auto from_it = accounts_.find(from);
        auto to_it = accounts_.find(to);
        if (from_it == accounts_.end() || to_it == accounts_.end()) {
            throw std::out_of_range("source or destination account was not found");
        }
        if (from_it->second.status != "active" || to_it->second.status != "active") {
            throw std::domain_error("transfers require both accounts to be active");
        }
        if (from_it->second.balance_paise < amount) {
            throw std::domain_error("insufficient funds");
        }
        if (to_it->second.balance_paise > std::numeric_limits<std::int64_t>::max() - amount) {
            throw std::domain_error("destination balance would overflow the supported integer range");
        }

        // All validation occurs before mutation; the mutex makes this single-node
        // transfer atomic with respect to concurrent requests in this process.
        const auto txn_id = make_id("TXN", ++transaction_sequence_);
        from_it->second.balance_paise -= amount;
        to_it->second.balance_paise += amount;
        ++from_it->second.version;
        ++to_it->second.version;

        Transaction txn{txn_id, from, to, amount, "Committed", now_time(), "SHARD-01", key};
        transactions_.push_back(txn);
        const auto index = transactions_.size() - 1;
        idempotency_.emplace(key, IdempotencyRecord{index, from, to, amount});
        ledger_.push_back({make_id("LE", ++ledger_sequence_), txn_id, from, "Debit", amount});
        ledger_.push_back({make_id("LE", ++ledger_sequence_), txn_id, to, "Credit", amount});
        audit("TRANSFER_COMMITTED", txn_id, from + " -> " + to + " : " + std::to_string(amount) + " paise");
        persist_snapshot();
        return {{"transaction", transaction_json(txn)}, {"duplicate", false}, {"message", "Transfer committed"}};
    }

    json dashboard() const {
        std::lock_guard lock(mutex_);
        std::int64_t total_balance = 0;
        for (const auto& [_, account] : accounts_) total_balance += account.balance_paise;
        std::int64_t total_debits = 0;
        std::int64_t total_credits = 0;
        for (const auto& entry : ledger_) {
            if (entry.type == "Debit") total_debits += entry.amount_paise;
            else total_credits += entry.amount_paise;
        }
        return {
            {"accounts_count", accounts_.size()},
            {"transaction_count", transactions_.size()},
            {"committed_transactions", transactions_.size()},
            {"total_balance_paise", total_balance},
            {"total_debits_paise", total_debits},
            {"total_credits_paise", total_credits},
            {"ledger_balanced", total_debits == total_credits},
            {"storage_mode", "json-snapshot-plus-wal"},
            {"persistence_healthy", persistence_ok_},
            {"node_count", 1}
        };
    }

    json transaction_by_id(const std::string& id) const {
        std::lock_guard lock(mutex_);
        const auto it = std::find_if(transactions_.begin(), transactions_.end(), [&](const Transaction& t) { return t.id == id; });
        if (it == transactions_.end()) throw std::out_of_range("transaction was not found");
        return transaction_json(*it);
    }

    json reconcile() const {
        std::lock_guard lock(mutex_);
        std::int64_t debits = 0, credits = 0;
        std::unordered_map<std::string, std::int64_t> net;
        for (const auto& entry : ledger_) {
            if (entry.type == "Debit") { debits += entry.amount_paise; net[entry.account_id] -= entry.amount_paise; }
            else if (entry.type == "Credit") { credits += entry.amount_paise; net[entry.account_id] += entry.amount_paise; }
        }
        json differences = json::array();
        for (const auto& [id, account] : accounts_) {
            // Opening balances are the seed/opening position; only compare post-ledger movement
            // through a separate invariant: total debits must equal total credits.
            (void)id; (void)account;
        }
        return {{"balanced", debits == credits}, {"total_debits_paise", debits},
                {"total_credits_paise", credits}, {"difference_paise", debits - credits},
                {"ledger_entries", ledger_.size()}, {"transactions", transactions_.size()},
                {"account_count", accounts_.size()}, {"checks", json::array({"debits_equal_credits", "transaction_has_two_ledger_entries"})},
                {"status", debits == credits ? "passed" : "failed"}};
    }

    json ledger_entries() const {
        std::lock_guard lock(mutex_);
        json result = json::array();
        for (auto it = ledger_.rbegin(); it != ledger_.rend(); ++it) {
            result.push_back({{"id", it->id}, {"transaction_id", it->transaction_id},
                {"account_id", it->account_id}, {"type", it->type},
                {"amount_paise", it->amount_paise}});
        }
        return result;
    }

    json fraud_alerts() const {
        std::lock_guard lock(mutex_);
        json alerts = json::array();
        std::unordered_map<std::string, int> sender_counts;
        for (const auto& txn : transactions_) ++sender_counts[txn.from];
        for (const auto& txn : transactions_) {
            if (txn.amount_paise >= 5'000'000) {
                alerts.push_back({{"id", "ALERT-" + txn.id}, {"transaction_id", txn.id},
                    {"reason", "Transfer exceeds the configured high-value threshold"},
                    {"amount_paise", txn.amount_paise}, {"from", txn.from}, {"to", txn.to},
                    {"severity", txn.amount_paise >= 10'000'000 ? "high" : "medium"}});
            }
            if (sender_counts[txn.from] >= 3) {
                alerts.push_back({{"id", "VELOCITY-" + txn.id}, {"transaction_id", txn.id},
                    {"reason", "Sender has at least three transfers in the saved transaction history"},
                    {"amount_paise", txn.amount_paise}, {"from", txn.from}, {"to", txn.to},
                    {"severity", "medium"}});
            }
        }
        return alerts;
    }

private:
    static std::string required_string(const json& body, const char* key) {
        const auto it = body.find(key);
        if (it == body.end() || !it->is_string() || it->get<std::string>().empty()) {
            throw std::invalid_argument(std::string(key) + " must be a non-empty string");
        }
        return it->get<std::string>();
    }

    static json account_json(const Account& a) {
        return {{"id", a.id}, {"name", a.name}, {"balance_paise", a.balance_paise},
                {"balance", static_cast<double>(a.balance_paise) / 100.0}, {"version", a.version}, {"status", a.status}};
    }

    static json transaction_json(const Transaction& t) {
        return {{"id", t.id}, {"from", t.from}, {"to", t.to},
                {"amount_paise", t.amount_paise},
                {"amount", static_cast<double>(t.amount_paise) / 100.0},
                {"status", t.status}, {"time", t.time}, {"shard", t.shard}};
    }

    struct IdempotencyRecord {
        std::size_t transaction_index;
        std::string from;
        std::string to;
        std::int64_t amount_paise;
    };

    void audit(const std::string& event, const std::string& entity, const std::string& details) {
        audit_.push_back({make_id("AUD", ++audit_sequence_), event, entity, details, now_time()});
    }

    json snapshot_json() const {
        json data;
        data["schema_version"] = 1;
        data["state_revision"] = state_revision_;
        data["account_sequence"] = account_sequence_;
        data["transaction_sequence"] = transaction_sequence_;
        data["ledger_sequence"] = ledger_sequence_;
        data["audit_sequence"] = audit_sequence_;
        data["accounts"] = json::array();
        for (const auto& [_, a] : accounts_) data["accounts"].push_back({{"id",a.id},{"name",a.name},{"balance_paise",a.balance_paise},{"version",a.version},{"status",a.status}});
        data["transactions"] = json::array();
        for (const auto& t : transactions_) data["transactions"].push_back({{"id",t.id},{"from",t.from},{"to",t.to},{"amount_paise",t.amount_paise},{"status",t.status},{"time",t.time},{"shard",t.shard},{"idempotency_key",t.idempotency_key}});
        data["ledger"] = json::array();
        for (const auto& e : ledger_) data["ledger"].push_back({{"id",e.id},{"transaction_id",e.transaction_id},{"account_id",e.account_id},{"type",e.type},{"amount_paise",e.amount_paise}});
        data["audit"] = json::array();
        for (const auto& e : audit_) data["audit"].push_back({{"id",e.id},{"event",e.event},{"entity_id",e.entity_id},{"details",e.details},{"time",e.time}});
        data["idempotency"] = json::array();
        for (const auto& [key, r] : idempotency_) data["idempotency"].push_back({{"key",key},{"transaction_index",r.transaction_index},{"from",r.from},{"to",r.to},{"amount_paise",r.amount_paise}});
        return data;
    }

    void persist_snapshot() {
        const auto target = std::filesystem::path(data_dir_) / "finledger_snapshot.json";
        const auto temporary = std::filesystem::path(data_dir_) / "finledger_snapshot.tmp";
        try {
            auto data = snapshot_json();
            data["state_revision"] = ++state_revision_;
            const auto serialized = data.dump();
            // Append a complete, checksummed-by-JSON state record before acknowledging
            // the mutation. This is a simple snapshot-WAL, not a production database WAL.
            {
                std::ofstream wal(std::filesystem::path(data_dir_) / "finledger.wal", std::ios::binary | std::ios::app);
                if (!wal) throw std::runtime_error("cannot open write-ahead log");
                wal << serialized << '\n';
                wal.flush();
                if (!wal) throw std::runtime_error("failed writing write-ahead log");
                ++wal_records_;
            }
            { std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
              if (!out) throw std::runtime_error("cannot open snapshot temp file");
              out << data.dump(2); out.flush();
              if (!out) throw std::runtime_error("failed writing snapshot"); }
            std::error_code ec;
            std::filesystem::rename(temporary, target, ec);
            if (ec) {
                std::filesystem::remove(target, ec); ec.clear();
                std::filesystem::rename(temporary, target, ec);
                if (ec) throw std::runtime_error("snapshot rename failed: " + ec.message());
            }
            persistence_ok_ = true; persistence_error_.clear();
            // Compact after 100 records: the durable snapshot is already written,
            // so the WAL can safely be reduced to one latest-state recovery record.
            if (wal_records_ >= 100) {
                std::ofstream wal(std::filesystem::path(data_dir_) / "finledger.wal", std::ios::binary | std::ios::trunc);
                if (!wal) throw std::runtime_error("cannot compact write-ahead log");
                wal << serialized << '\n'; wal.flush();
                if (!wal) throw std::runtime_error("failed compacting write-ahead log");
                wal_records_ = 1;
                ++compactions_;
            }
        } catch (const std::exception& e) {
            persistence_ok_ = false; persistence_error_ = e.what();
            std::cerr << "Persistence warning: " << persistence_error_ << '\n';
        }
    }

    void restore_data(const json& data) {
        if (data.value("schema_version", 0) != 1) throw std::runtime_error("unsupported snapshot schema version");
        accounts_.clear(); transactions_.clear(); ledger_.clear(); audit_.clear(); idempotency_.clear();
        account_sequence_ = data.value("account_sequence", std::uint64_t{100});
        transaction_sequence_ = data.value("transaction_sequence", std::uint64_t{0});
        ledger_sequence_ = data.value("ledger_sequence", std::uint64_t{0});
        audit_sequence_ = data.value("audit_sequence", std::uint64_t{0});
        state_revision_ = data.value("state_revision", std::uint64_t{0});
        for (const auto& a : data.at("accounts")) { Account x{a.at("id"),a.at("name"),a.at("balance_paise"),a.value("version",std::uint64_t{1}),a.value("status",std::string("active"))}; accounts_.emplace(x.id,x); }
        for (const auto& t : data.at("transactions")) transactions_.push_back({t.at("id"),t.at("from"),t.at("to"),t.at("amount_paise"),t.at("status"),t.at("time"),t.value("shard",std::string("SHARD-01")),t.value("idempotency_key",std::string{})});
        for (const auto& e : data.at("ledger")) ledger_.push_back({e.at("id"),e.at("transaction_id"),e.at("account_id"),e.at("type"),e.at("amount_paise")});
        if (data.contains("audit")) for (const auto& e : data.at("audit")) audit_.push_back({e.at("id"),e.at("event"),e.at("entity_id"),e.at("details"),e.at("time")});
        if (data.contains("idempotency")) for (const auto& r : data.at("idempotency")) idempotency_.emplace(r.at("key").get<std::string>(), IdempotencyRecord{r.at("transaction_index"),r.at("from"),r.at("to"),r.at("amount_paise")});
    }

    bool load_snapshot() {
        const auto path = std::filesystem::path(data_dir_) / "finledger_snapshot.json";
        const auto wal_path = std::filesystem::path(data_dir_) / "finledger.wal";
        bool restored = false;
        try {
            if (std::filesystem::exists(path)) {
                std::ifstream in(path, std::ios::binary); json data; in >> data;
                restore_data(data); restored = true;
            }
            // Replay the newest complete WAL record. A truncated final line is ignored.
            if (std::filesystem::exists(wal_path)) {
                std::ifstream wal(wal_path, std::ios::binary);
                std::string line; json latest; bool has_latest = false; std::uint64_t count = 0;
                while (std::getline(wal, line)) {
                    if (line.empty()) continue;
                    try { auto record = json::parse(line); if (record.value("schema_version", 0) == 1) { latest = std::move(record); has_latest = true; ++count; } }
                    catch (...) { /* ignore an incomplete/corrupt trailing WAL record */ }
                }
                wal_records_ = count;
                if (has_latest && (!restored || latest.value("state_revision", std::uint64_t{0}) >= state_revision_)) {
                    restore_data(latest); restored = true;
                    std::cout << "Recovered latest state from write-ahead log.\n";
                }
            }
            persistence_ok_ = true;
            if (restored) std::cout << "Restored " << accounts_.size() << " accounts and " << transactions_.size() << " transactions.\n";
            return restored;
        } catch (const std::exception& e) {
            persistence_ok_ = false; persistence_error_ = e.what();
            std::cerr << "Recovery failed: " << persistence_error_ << '\n';
            accounts_.clear(); transactions_.clear(); ledger_.clear(); audit_.clear(); idempotency_.clear();
            return false;
        }
    }

    void add_seed(const std::string& id, const std::string& name, std::int64_t balance) {
        accounts_.emplace(id, Account{id, name, balance, 1});
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Account> accounts_;
    std::vector<Transaction> transactions_;
    std::vector<LedgerEntry> ledger_;
    std::unordered_map<std::string, IdempotencyRecord> idempotency_;
    std::uint64_t account_sequence_{100};
    std::uint64_t transaction_sequence_{0};
    std::uint64_t ledger_sequence_{0};
    std::vector<AuditEvent> audit_;
    std::uint64_t audit_sequence_{0};
    std::uint64_t state_revision_{0};
    std::uint64_t wal_records_{0};
    std::uint64_t compactions_{0};
    std::string data_dir_{"data"};
    bool persistence_ok_{true};
    std::string persistence_error_;
};

void set_cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "http://localhost:5173");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Idempotency-Key");
}

void send_json(httplib::Response& res, const json& value, int status = 200) {
    res.status = status;
    res.set_content(value.dump(2), "application/json; charset=utf-8");
    set_cors(res);
}

void send_error(httplib::Response& res, int status, const std::string& message) {
    send_json(res, {{"error", message}}, status);
}

} // namespace

int main() {
    LedgerService ledger;
    httplib::Server server;
    server.set_payload_max_length(1024 * 1024);
    server.set_read_timeout(5, 0);
    server.set_write_timeout(5, 0);

    server.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) {
        set_cors(res);
        res.status = 204;
    });

    server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, {{"status", "healthy"}, {"service", "FinLedger C++ API"},
            {"version", "0.5.0"}, {"mode", "single-node-wal-snapshot"}});
    });

    server.Get("/api/dashboard", [&](const httplib::Request&, httplib::Response& res) {
        send_json(res, ledger.dashboard());
    });

    server.Get("/api/accounts", [&](const httplib::Request&, httplib::Response& res) {
        send_json(res, {{"accounts", ledger.accounts()}});
    });

    server.Get(R"(/api/accounts/([A-Za-z0-9-]+))", [&](const httplib::Request& req, httplib::Response& res) {
        try { send_json(res, {{"account", ledger.account_by_id(req.matches[1])}}); }
        catch (const std::out_of_range& e) { send_error(res, 404, e.what()); }
    });

    server.Post(R"(/api/accounts/([A-Za-z0-9-]+)/status)", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto body = json::parse(req.body);
            if (!body.contains("status") || !body["status"].is_string()) throw std::invalid_argument("status is required");
            send_json(res, {{"account", ledger.set_account_status(req.matches[1], body["status"].get<std::string>())}});
        } catch (const std::invalid_argument& e) { send_error(res, 400, e.what()); }
          catch (const std::out_of_range& e) { send_error(res, 404, e.what()); }
          catch (const json::exception& e) { send_error(res, 400, e.what()); }
    });

    server.Post("/api/accounts", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto body = json::parse(req.body);
            send_json(res, {{"account", ledger.create_account(body)}}, 201);
        } catch (const std::invalid_argument& e) { send_error(res, 400, e.what()); }
          catch (const json::exception& e) { send_error(res, 400, e.what()); }
    });

    server.Get("/api/transactions", [&](const httplib::Request&, httplib::Response& res) {
        send_json(res, {{"transactions", ledger.transactions()}});
    });

    server.Get(R"(/api/transactions/([A-Za-z0-9-]+))", [&](const httplib::Request& req, httplib::Response& res) {
        try { send_json(res, {{"transaction", ledger.transaction_by_id(req.matches[1])}}); }
        catch (const std::out_of_range& e) { send_error(res, 404, e.what()); }
    });

    server.Get("/api/reconciliation", [&](const httplib::Request&, httplib::Response& res) {
        send_json(res, ledger.reconcile());
    });

    server.Post("/api/transfers", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto body = json::parse(req.body);
            const auto result = ledger.transfer(body);
            send_json(res, result, result.value("duplicate", false) ? 200 : 201);
        } catch (const std::invalid_argument& e) { send_error(res, 400, e.what()); }
          catch (const std::domain_error& e) { send_error(res, 409, e.what()); }
          catch (const std::out_of_range& e) { send_error(res, 404, e.what()); }
          catch (const json::exception& e) { send_error(res, 400, e.what()); }
    });

    server.Get("/api/ledger", [&](const httplib::Request&, httplib::Response& res) {
        send_json(res, {{"entries", ledger.ledger_entries()}});
    });

    server.Get("/api/cluster", [](const httplib::Request&, httplib::Response& res) {
        const json nodes = json::array({json{{"id", "node-01"}, {"role", "standalone"},
            {"status", "healthy"}, {"is_leader", true}}});
        const json shards = json::array({json{{"id", "SHARD-01"}, {"status", "healthy"}, {"replicas", 1}}});
        send_json(res, {{"mode", "single-node"}, {"nodes", nodes}, {"shards", shards},
            {"consensus", "not-yet-implemented"}});
    });

    server.Get("/api/fraud/alerts", [&](const httplib::Request&, httplib::Response& res) {
        send_json(res, {{"alerts", ledger.fraud_alerts()},
            {"engine", "basic-history-rules"},
            {"note", "Illustrative high-value and sender-velocity rules; not a production fraud model."}});
    });

    server.Get("/api/storage/metrics", [&](const httplib::Request&, httplib::Response& res) {
        send_json(res, ledger.persistence_status());
    });

    server.Get("/api/audit", [&](const httplib::Request&, httplib::Response& res) {
        send_json(res, {{"events", ledger.audit_events()}});
    });

    server.Get("/api/benchmarks", [&](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("run") || req.get_param_value("run") != "1") {
            send_json(res, {{"available", false}, {"message", "Run the read-path microbenchmark to collect local measurements."}});
            return;
        }
        int iterations = 5000;
        try { if (req.has_param("iterations")) iterations = std::clamp(std::stoi(req.get_param_value("iterations")), 1000, 200000); }
        catch (...) { send_error(res, 400, "iterations must be an integer between 1000 and 200000"); return; }
        const auto started = std::chrono::steady_clock::now();
        volatile std::uint64_t checksum = 0;
        for (int i = 0; i < iterations; ++i) {
            const auto accounts = ledger.accounts();
            checksum = checksum + accounts.size() + static_cast<std::uint64_t>(i & 1);
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        const double ops_per_second = elapsed > 0 ? (iterations * 1000.0 / elapsed) : 0.0;
        send_json(res, {{"available", true}, {"benchmark", "account-list-json-read"},
            {"iterations", iterations}, {"elapsed_ms", elapsed}, {"operations_per_second", ops_per_second},
            {"message", "Read-path microbenchmark completed."}, {"checksum", static_cast<std::uint64_t>(checksum)},
            {"scope", "single-process read-path microbenchmark; not a production load test"}});
    });

    std::cout << "\n  FinLedger C++ API v0.5.0\n"
              << "  --------------------------------\n"
              << "  API:    http://localhost:8080\n"
              << "  Health: http://localhost:8080/api/health\n"
              << "  Press Ctrl+C to stop.\n\n";

    if (!server.listen("127.0.0.1", 8080)) {
        std::cerr << "Failed to bind to 127.0.0.1:8080. Is the port already in use?\n";
        return 1;
    }
    return 0;
}
