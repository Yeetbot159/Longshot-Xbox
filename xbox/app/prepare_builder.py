"""Build and load the native Python helpers in the staged Xbox package."""
import os
import sys

tools = os.path.abspath(sys.argv[1])
sys.path.insert(0, os.path.join(tools, "nx2"))
sys.path.insert(0, os.path.join(tools, "third_party", "a64-kit", "lift"))

import build_emit
import a64emit
import zbic

build_emit.build(force=True)
a64emit.lib()
zbic._build(force=True)
zbic.lib()
