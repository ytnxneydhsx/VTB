# Project support files

This directory keeps development support material outside the main C++ source
layout:

- `patches/`: reproducible patches applied to external source;
- `requirements/`: Python dependency lists used by diagnostic tooling;
- `tools/`: Python and C++ development or conversion utilities;
- `third_party/`: vendored or installed external source trees.

Product C++ code remains in `apps/`, `include/`, and `src/`. Tests, examples,
configuration, and design documentation keep their conventional top-level
locations.
