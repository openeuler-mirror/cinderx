# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Assign a stable GCC seed to each CMake compile and link output."""

import hashlib
import os
from pathlib import Path
import sys


def main() -> None:
    build_dir, *command = sys.argv[1:]
    if "-o" in command:
        output = Path(command[command.index("-o") + 1]).resolve()
        phase = "compile" if "-c" in command else "link"
        target = os.path.relpath(output, build_dir)
        seed = hashlib.sha256(f"{phase}:{target}".encode()).hexdigest()[:16]
        command.append(f"-frandom-seed=0x{seed}")
    os.execvp(command[0], command)


if __name__ == "__main__":
    main()
