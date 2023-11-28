#!/usr/bin/env python3

# pexpect script to Install NetBSD base Domain
#
# Copyright (C) 2021 Andrew Cagney
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.

# Possibly useful reference:
# http://meta.libera.cc/2020/12/quick-netbsd-serial-console-install-on.html

import pexpect
import sys
import time
import os

FILTER_OUTPUT = False

def i(child):
    '''go interactive then quit'''
    child.logfile = None
    child.interact()
    sys.exit(0)

def rs(child, r, s):
    child.expect(r, timeout=None)
    for c in s:
        child.send(c)

def c(child, s):
    child.expect('\n# ', timeout=None)
    time.sleep(1)
    for c in s:
        child.send(c)
    child.send('\n')

# boot in single user mode (/ is RO)

def install_base(child, param):
    rs(child, 'seconds', '2')
    rs(child, 'Enter pathname of shell or RETURN for /bin/sh:', '\n')
    # the above has only 4 seconds

    # Configure the created system using base.sh

    c(child, 'mount -rt cd9660 /dev/cd1 /mnt')
    c(child, '/bin/sh -x /mnt/base.sh')

    c(child, 'umount /targetroot')
    c(child, 'umount /mnt')
    c(child, 'halt -p')

    sys.exit(child.wait())
