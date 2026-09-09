# SwarmSync Operations Runbook

SwarmSync V1 is intended for a trusted local network or a private VPN. The tracker stores only short-lived peer metadata; file bytes move directly between peers.

## Start a private swarm

Set the same secret token in every tracker, seed, and download shell. Keep it outside the repository.

```bash
export SWARMSYNC_TOKEN='use-a-unique-private-token'
```

Start the tracker on a host reachable by the peers:

```bash
./build/swarmsync-tracker \
  --port 47000 --bind 0.0.0.0 --ttl-seconds 30 \
  --max-connections 32 --workers 4 \
  --max-swarms 1024 --max-peers-per-swarm 256 --max-response-peers 128
```

`--max-connections` bounds queued and active tracker clients, `--workers` bounds request handlers, and the remaining options bound tracker metadata and each discovery response. Size them for the trusted deployment, then monitor rejected `busy` or `capacity` responses. Allow the tracker TCP port and each peer's configured listening port through the private-network firewall. Do not expose these unauthenticated TCP channels to the public Internet.

## Normal operating checks

- Tracker startup prints its address and presence TTL.
- A seeder prints its peer ID, swarm ID, and listening address after its first tracker announcement succeeds.
- A downloader prints its verified output path, completed chunk count, retry events, and elapsed time.
- The final file exists only after the whole-file SHA-256 matches the manifest.

Use separate log files or your process supervisor's log capture for the tracker, every seeder, and every downloader. Secrets are not printed by the application and should not be included in log collection commands.

## Restart behavior

1. Stop a downloader with `Ctrl+C` or restart its process.
2. Keep both `<file>.part` and `<swarm-id>.resume` in the selected output directory.
3. Run the same download command again with the same manifest and output directory.
4. SwarmSync re-hashes every resume-marked chunk before serving or skipping it, clears any invalid marks, and requests only missing or invalid chunks.

Do not manually edit the resume file. It is atomically written after each verified chunk. If the resume state exists but its matching partial file does not, SwarmSync stops instead of guessing which chunks are safe.

## Incident handling

| Symptom | Meaning | Safe response |
| --- | --- | --- |
| `Tracker rejected request` | Token mismatch, malformed request, wrong swarm parameters, or a `busy`/`capacity` limit | Confirm every process uses the same token and manifest. For `busy` or `capacity`, reduce client churn or increase the corresponding tracker limit after checking memory and connection capacity. |
| `Unable to fetch chunk` | No active peer advertises a needed chunk, or peer connections failed | Keep at least one complete seeder running. Check private firewall rules and configured ports. |
| `Final full-file SHA-256 verification failed` | The partial output does not match the immutable manifest | Preserve the partial file and logs for inspection; do not publish it. Re-run after confirming a known-good seeder and manifest. |
| `Resume state does not match` | A manifest was changed while reusing an old output directory | Use a new output directory for the new manifest, or remove only the matching old partial and resume files after confirming they are no longer needed. |
| `Existing partial file does not match` | Disk state was changed outside SwarmSync | Stop the download and investigate disk contents before removing anything. |
| `Address already in use` | Another process owns the tracker or peer port | Select an unused port and update that process's command or environment value. |
| Disk write error | Output volume is unavailable or full | Free space or select another output directory; preserve the valid resume state if possible. |

## Production deployment boundary

The V1 shared token protects membership only on a trusted local/LAN network. Before using SwarmSync across untrusted networks, add all of the following:

- TLS 1.3 or mutual TLS for tracker and peer traffic.
- Per-peer identity, short-lived signed membership tokens, and token rotation.
- Firewall allowlists, rate limits, connection quotas, structured audit logging, and monitoring.
- A signed manifest distribution channel and approval process for what may be seeded.
- Threat modeling for NAT traversal and any relay service before enabling public reachability.

Until these controls exist, run the tracker and peers only behind a VPN or within a private network you control.
