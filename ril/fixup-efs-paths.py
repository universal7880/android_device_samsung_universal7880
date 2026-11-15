# SPDX-License-Identifier: Apache-2.0

import sys
from pathlib import Path


PATHS = {
    b"/efs/FactoryApp/serial_no": b"/mnt/vendor/ril_serial",
    b"/efs/imei/mps_code.dat": b"/mnt/imei/mps_code.dat",
    b"/efs/imei/omcnw_code.dat": b"/mnt/imei/omcnw_code.dat",
    b"/efs/imei/omcnw_code2.dat": b"/mnt/imei/omcnw_code2.dat",
}


def fixup(blob):
    original = blob.read_bytes()
    # Accept binaries extracted from builds that used a symlink to mps_code.dat.
    mps_path = b"/efs/imei/mps_code.dat\0"
    previous_mps_path = b"/mnt/vendor/ril_mps".ljust(len(mps_path), b"\0")
    data = original.replace(previous_mps_path, mps_path)
    for legacy_path, vendor_path in PATHS.items():
        # Preserve the offsets of the other strings in the proprietary ELF.
        if len(vendor_path) > len(legacy_path):
            raise ValueError(f"{vendor_path.decode()} does not fit in the original ELF string")
        old = legacy_path + b"\0"
        new = vendor_path.ljust(len(old), b"\0")
        if data.count(old) == 1 and new not in data:
            data = data.replace(old, new)
        elif old in data or data.count(new) != 1:
            raise ValueError(f"{blob}: unexpected occurrences of {legacy_path.decode()}")
    if data != original:
        blob.write_bytes(data)


if __name__ == "__main__":
    for path in sys.argv[1:]:
        fixup(Path(path))
