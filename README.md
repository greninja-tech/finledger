# FinLedger — consolidated systems-engineering build (v0.5.0)

FinLedger is an educational financial-ledger project with a C++20 HTTP API and a React + TypeScript frontend. The API is a **single-node application**. A separate dependency-light C++ Systems Lab demonstrates database and distributed-systems concepts without pretending that an in-process simulation is a production cluster.

## Financial application capabilities

- Account creation, lookup, freeze/unfreeze and transfer validation.
- Integer-paise accounting, mutex-serialized single-process transfers, double-entry ledger and idempotency keys.
- JSON snapshot plus append-only full-state recovery log, restart recovery, and threshold-based WAL compaction.
- Reconciliation endpoint, audit events, rule-based fraud alerts, and a local read-path microbenchmark.
- React dashboard with account, transfer, transaction, ledger, fraud, storage, cluster-status and reconciliation views.

## Systems Lab (new in v0.5.0)

The independent `finledger_systems_lab` target contains working educational modules:

- **LSM key/value store:** append-only WAL, sorted immutable SSTables, sorted in-memory MemTable, point-lookup index, Bloom filter, tombstones, flush, restart recovery and compaction.
- **MVCC store:** versioned values and snapshot reads.
- **Logical sharding:** deterministic key-to-shard routing.
- **Raft simulator:** majority-gated writes, leader election and leader-failure re-election in a deterministic in-process simulation.

These Systems Lab modules are independently executable and tested. They are not wired into the HTTP ledger's account/transfer persistence path. In particular, the Raft simulator is **not** networked replication; it is a teaching model of the protocol flow.

## Build on Windows / MSYS2 UCRT64

From `backend`:

```bash
cmake -S . -B build-mingw -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build-mingw
```

This builds the HTTP server, Systems Lab executable and Systems Lab tests. Start the HTTP server:

```bash
./build-mingw/finledger_server.exe
```

In a second terminal:

```powershell
cd frontend
npm install
npm run build
npm run dev
```

Open `http://localhost:5173`.

## Run the Systems Lab

From `backend` in MSYS2 UCRT64:

```bash
./build-mingw/finledger_systems_lab.exe
./build-mingw/finledger_systems_lab.exe lsm systems_lab_data
./build-mingw/finledger_systems_lab.exe mvcc
./build-mingw/finledger_systems_lab.exe shard
./build-mingw/finledger_systems_lab.exe raft
```

Run automated C++ tests:

```bash
ctest --test-dir build-mingw --output-on-failure
```

## Current limitations — do not present as production banking software

- The HTTP ledger's JSON snapshot/WAL remains separate from the Systems Lab LSM store; the latter is not yet the ledger's authoritative storage engine.
- The HTTP API is single-process and has no multi-machine replication, networked Raft, distributed transactions, or consensus-backed commit path.
- Logical sharding is a deterministic routing demonstration, not deployed data partitioning.
- MVCC is an independent teaching component, not the transaction isolation mechanism in the HTTP ledger (which uses a process mutex).
- The Raft module simulates a small cluster in one process; it does not implement TCP/gRPC transport, persistent Raft logs, heartbeats, timeouts, or real elections over a network.
- The HTTP WAL flushes C++ streams but does not provide database-grade fsync, checksums, segmented logs, or fully crash-consistent transactional rollback.
- Authentication, authorization, TLS, secrets management, production fraud modelling, and production load testing are not included.

Do not use this project to hold real funds or sensitive financial information.
