# Research

`evidence/` holds the curated Phase 1 evidence files: findings, with the quotes, locations and measurements behind them.

Raw tool outputs, probe and setup scripts, and cached downloads are kept outside this repo and are not published. Paths in the
evidence files that start with these prefixes are labels into that private archive, not paths in this repo:

- `CACHE/...` and `CACHE-D/...`: cached sources and downloads, and shorter forms of them such as `dh-docs/...` and `simd-repos/...`.
- `raw/...`, `src/...` and `bin/...` in `machine-probe.md`.
- `output/...` and `scripts/...` in `system-feasibility.md`.
- `debian-check/...` in `debian-host.md`.

`measure.sh` and `heavy.sh` are locks on the measurement machines. They are also not part of this repo.
