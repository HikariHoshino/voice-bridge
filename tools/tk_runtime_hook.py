"""Point the packaged application at its bundled Tcl/Tk script libraries."""

import os
import sys


bundle_root = getattr(sys, "_MEIPASS", None)
if bundle_root:
    os.environ["TCL_LIBRARY"] = os.path.join(bundle_root, "_tcl_data")
    os.environ["TK_LIBRARY"] = os.path.join(bundle_root, "_tk_data")
