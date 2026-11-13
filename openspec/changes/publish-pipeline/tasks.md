## 1. One publish job
- [ ] 1.1 Collect every board's artifacts in one job after the matrix
- [ ] 1.2 Regenerate `manifest.json` once and push one commit
- [ ] 1.3 Drop the rebase-and-retry from the per-board publish step

## 2. Separate builds branch (decide first)
- [ ] 2.1 Decide with the owner whether a `builds` branch is wanted
- [ ] 2.2 If yes: change the flasher's manifest URL and iot.ndw.ai first, then publish there
