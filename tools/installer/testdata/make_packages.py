#!/usr/bin/env python3
"""Builds the installer tests' packages: small zips laid out like the
Windows release package, and ones that are not packages.

    python3 tools/installer/testdata/make_packages.py
"""
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
FIXED = (2020, 1, 1, 0, 0, 0)


def package(path, top, files):
    with zipfile.ZipFile(os.path.join(HERE, path), 'w', zipfile.ZIP_DEFLATED) as z:
        for d in sorted({top + '/'} | {top + '/' + os.path.dirname(n) + '/' for n in files if '/' in n}):
            z.writestr(zipfile.ZipInfo(d, FIXED), b'')
        for n, data in files.items():
            z.writestr(zipfile.ZipInfo(top + '/' + n, FIXED), data, compress_type=zipfile.ZIP_DEFLATED)


GOOD = {
    'lib/cmake/ClannectFramework/ClannectFrameworkConfig.cmake': b'# test package\n',
    'lib/libcfw-core.a': b'!<arch>\n' + bytes(range(256)) * 20,
    'include/cfw/core/String.h': b'#pragma once\n',
    'bin/cfw-ui-gallery.exe': b'MZ' + bytes(1000),
    'license.md': b'Open Use License\n',
}

package('ClannectFramework-0.9.0-windows-x64-mingw.zip', 'ClannectFramework-0.9.0-windows-x64-mingw', GOOD)
package('ClannectFramework-1.0.0-rc.1-windows-x64-mingw.zip', 'ClannectFramework-1.0.0-rc.1-windows-x64-mingw', GOOD)
package('wrong-folder.zip', 'SomethingElse-1.0.0', GOOD)
package('no-cmake.zip', 'ClannectFramework-0.9.0-windows-x64-mingw',
        {k: v for k, v in GOOD.items() if not k.startswith('lib/cmake')})
print('packages written')
