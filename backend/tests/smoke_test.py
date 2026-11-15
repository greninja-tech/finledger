"""End-to-end smoke test for a locally built FinLedger server.
Run: python tests/smoke_test.py ./build-mingw/finledger_server.exe
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

EXE = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build-mingw/finledger_server.exe").resolve()
BASE = "http://127.0.0.1:8080"


def call(path, method="GET", body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(BASE + path, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as response:
            return response.status, json.loads(response.read().decode())
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode())


def start(data_dir):
    env = os.environ.copy()
    env["FINLEDGER_DATA_DIR"] = str(data_dir)
    proc = subprocess.Popen([str(EXE)], env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(50):
        try:
            if call("/api/health")[0] == 200:
                return proc
        except Exception:
            pass
        time.sleep(0.1)
    proc.terminate()
    raise RuntimeError("FinLedger server did not start")


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def main():
    if not EXE.exists():
        raise SystemExit(f"Server executable not found: {EXE}")
    with tempfile.TemporaryDirectory(prefix="finledger-test-") as tmp:
        data_dir = pathlib.Path(tmp)
        proc = start(data_dir)
        try:
            assert call("/api/health")[0] == 200
            accounts = call("/api/accounts")[1]["accounts"]
            assert len(accounts) >= 2
            status, result = call("/api/transfers", "POST", {
                "from_account": accounts[0]["id"], "to_account": accounts[1]["id"],
                "amount_paise": 100, "idempotency_key": "SMOKE-TEST-KEY"})
            assert status == 201, result
            assert call("/api/transfers", "POST", {
                "from_account": accounts[0]["id"], "to_account": accounts[1]["id"],
                "amount_paise": 100, "idempotency_key": "SMOKE-TEST-KEY"})[1]["duplicate"] is True
            dashboard = call("/api/dashboard")[1]
            assert dashboard["ledger_balanced"] is True
            assert call("/api/audit")[0] == 200
            assert call("/api/reconciliation")[1]["balanced"] is True
            txn_id = result["transaction"]["id"]
            assert call(f"/api/transactions/{txn_id}")[1]["transaction"]["id"] == txn_id
            account_id = accounts[0]["id"]
            assert call(f"/api/accounts/{account_id}/status", "POST", {"status": "frozen"})[0] == 200
            frozen_accounts = call("/api/accounts")[1]["accounts"]
            assert next(a for a in frozen_accounts if a["id"] == account_id)["status"] == "frozen"
            rejected = call("/api/transfers", "POST", {"from_account": account_id, "to_account": accounts[1]["id"], "amount_paise": 100, "idempotency_key": "FROZEN-ACCOUNT-TEST"})
            assert rejected[0] == 409
            assert call(f"/api/accounts/{account_id}/status", "POST", {"status": "active"})[0] == 200
            benchmark = call("/api/benchmarks?run=1&iterations=1000")[1]
            assert benchmark["available"] is True and benchmark["operations_per_second"] > 0
            assert (data_dir / "finledger.wal").exists()
            assert (data_dir / "finledger_snapshot.json").exists()
        finally:
            stop(proc)
        proc = start(data_dir)
        try:
            assert call("/api/dashboard")[1]["transaction_count"] == 1
            assert call("/api/accounts")[1]["accounts"][0]["balance_paise"] == accounts[0]["balance_paise"] - 100
        finally:
            stop(proc)
    print("PASS: health, transfer, idempotency, reconciliation, transaction lookup, account freeze, benchmark, WAL/snapshot, restart recovery")


if __name__ == "__main__":
    main()
