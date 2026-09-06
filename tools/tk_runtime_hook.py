"""Point bundled Tcl/Tk at the script libraries restored after collection."""

import os
import sys


bundle_root = getattr(sys, "_MEIPASS", None)
if bundle_root:
    os.environ["TCL_LIBRARY"] = os.path.join(bundle_root, "_tcl_data")
    os.environ["TK_LIBRARY"] = os.path.join(bundle_root, "_tk_data")
