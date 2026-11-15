# FinLedger Web

A polished React + TypeScript demo frontend for the FinLedger distributed financial ledger project.

## Run locally

1. Install Node.js 18+.
2. Open this folder in a terminal.
3. Run:

```bash
npm install
npm run dev
```

4. Open the local URL printed by Vite.

## Build

```bash
npm run build
npm run preview
```

## What's included

- Dark fintech dashboard and responsive sidebar
- Accounts, transfers, transaction explorer and ledger pages
- Fraud monitoring interface
- Cluster topology and storage engine visualizations
- Failure simulation playground
- Benchmark charts
- Search / quick navigation (click Search or press Ctrl/Cmd+K)
- Demo transfer flow with local UI state

## Important integration note

This package is a frontend showcase using clearly labeled demo/sample data. It does not connect to your C++ FinLedger engine yet and does not move real money. The Live mode switch is only a UI placeholder. To connect the actual system, implement API endpoints in your C++ service/API gateway and wire them in `src/App.tsx` (ideally through a separate `src/services/api.ts` module). Replace sample metrics and simulation results with measured backend responses.

## Stack

React, TypeScript, Vite, Recharts, Lucide React, CSS.
