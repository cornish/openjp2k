# Contributing to openjp2k

## Licensing

This repository is a fork of [uclouvain/openjpeg](https://github.com/uclouvain/openjpeg).
The original codebase is BSD-2-Clause; new and modified code in this fork
is Apache-2.0. See [`NOTICE`](NOTICE) for the full picture.

### SPDX headers

Every source file should declare its license inline using an
[SPDX-License-Identifier](https://spdx.dev/ids/) so license-scanning
tools don't have to guess. Two lines at the top of the file:

**New files you author (C / C++ / Go / Java / Rust / JS):**

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 <Your Name>
```

**New files in `#`-commented languages (Python, shell, YAML, CMake):**

```python
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 <Your Name>
```

**New files in block-only-comment languages (CSS, WGSL, GLSL):**

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 <Your Name> */
```

**Files inherited from upstream OpenJPEG:** leave the existing BSD-2
header intact. If you make non-trivial modifications, append your
copyright line under the existing notice; the SPDX identifier stays
`BSD-2-Clause`:

```c
/*
 * (existing upstream BSD-2 header, unchanged)
 * Copyright (c) 2002-2014, Universite catholique de Louvain (UCL), Belgium
 * ...
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Modifications copyright 2026 <Your Name>
 */
```

The SPDX identifier in inherited files matches the original license
(BSD-2-Clause), not Apache-2.0 — because the file as a whole remains
under its original license even after edits.

## Upstream tracking

Periodically pull changes from upstream OpenJPEG:

```sh
git fetch upstream
git merge upstream/master   # upstream's default branch is master; ours is main
```

Resolve conflicts as needed. Most conflicts will be on files we've
modified for performance reasons.
