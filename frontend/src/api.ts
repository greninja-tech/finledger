const API_BASE = (import.meta.env.VITE_API_BASE_URL ?? 'http://localhost:8080/api').replace(/\/$/, '');

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  let response: Response;
  try {
    response = await fetch(`${API_BASE}${path}`, {
      ...init,
      headers: { Accept: 'application/json', ...(init?.body ? { 'Content-Type': 'application/json' } : {}), ...(init?.headers ?? {}) },
    });
  } catch {
    throw new Error('Cannot reach the C++ API. Check that finledger_server.exe is running on port 8080.');
  }
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(payload.error ?? `API request failed (${response.status})`);
  return payload as T;
}

export type ApiAccount = { id: string; name: string; balance_paise: number; balance: number; version: number; status: "active" | "frozen" };
export type ApiTransaction = { id: string; from: string; to: string; amount: number; amount_paise: number; status: string; time: string; shard: string };
export type LedgerEntry = { id: string; transaction_id: string; account_id: string; type: 'Debit' | 'Credit'; amount_paise: number };
export type ClusterNode = { id: string; role: string; status: string; is_leader: boolean };

export const finledgerApi = {
  health: () => request<{ status: string; service: string; version: string; mode: string }>('/health'),
  dashboard: () => request<{ accounts_count: number; transaction_count: number; committed_transactions: number; total_balance_paise: number; total_debits_paise: number; total_credits_paise: number; ledger_balanced: boolean; storage_mode: string; node_count: number }>('/dashboard'),
  accounts: async () => (await request<{ accounts: ApiAccount[] }>('/accounts')).accounts,
  accountStatus: async (id: string, status: "active" | "frozen") => (await request<{ account: ApiAccount }>(`/accounts/${encodeURIComponent(id)}/status`, { method: 'POST', body: JSON.stringify({ status }) })).account,
  reconciliation: () => request<{ balanced: boolean; total_debits_paise: number; total_credits_paise: number; difference_paise: number; ledger_entries: number; transactions: number; account_count: number; status: string }>('/reconciliation'),
  createAccount: async (body: { name: string; opening_balance_paise: number }) => (await request<{ account: ApiAccount }>('/accounts', { method: 'POST', body: JSON.stringify(body) })).account,
  transactions: async () => (await request<{ transactions: ApiTransaction[] }>('/transactions')).transactions,
  transfer: (body: { from_account: string; to_account: string; amount_paise: number; idempotency_key: string }) =>
    request<{ transaction: ApiTransaction; duplicate: boolean; message: string }>('/transfers', { method: 'POST', body: JSON.stringify(body) }),
  ledger: async () => (await request<{ entries: LedgerEntry[] }>('/ledger')).entries,
  fraudAlerts: () => request<{ alerts: Array<{ id: string; transaction_id: string; reason: string; amount_paise: number; from: string; to: string; severity: string }>; engine: string }>('/fraud/alerts'),
  cluster: () => request<{ mode: string; nodes: ClusterNode[]; shards: Array<{ id: string; status: string; replicas: number }>; consensus: string }>('/cluster'),
  storage: () => request<{ engine: string; wal_records: number; memtable_entries: number; sstable_count: number; compactions: number; durability: string; state_revision?: number; wal_file?: string; snapshot_file?: string; last_error?: string; audit_events?: number }>('/storage/metrics'),
  audit: async () => (await request<{ events: Array<{ id: string; event: string; entity_id: string; details: string; time: string }> }>('/audit')).events,
  benchmarks: () => request<{ available: boolean; message: string }>('/benchmarks'),
  runBenchmark: (iterations = 5000) => request<{ available: boolean; benchmark: string; iterations: number; elapsed_ms: number; operations_per_second: number; checksum: number; scope: string; message: string }>(`/benchmarks?run=1&iterations=${iterations}`),
};
