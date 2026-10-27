# Third-party source snapshots

This directory vendors the source used by the RK3588 Live2D experiments so the
tested Godot extension and Ayagami core can be rebuilt together.

## Ayagami

- `ayagami/` originates from <https://github.com/AyagamiDev/ayagami-gd>.
- `ayagami-core/` originates from <https://github.com/AyagamiDev/ayagami>.
- The received RK source snapshot did not contain Git metadata, so an exact
  upstream commit cannot be claimed from the snapshot alone.
- Upstream license and copyright files are retained in each source tree.
- Local performance changes are kept as later commits in this repository so
  the imported baseline and each optimization step remain reviewable.

Generated libraries, build directories, model weights, and runtime outputs are
not vendored here.
