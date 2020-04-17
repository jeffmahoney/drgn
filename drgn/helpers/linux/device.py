# Copyright (c) Facebook, Inc. and its affiliates.
# SPDX-License-Identifier: GPL-3.0+

"""
Devices
-------

The ``drgn.helpers.linux.device`` module provides helpers for working with
Linux devices, including the kernel encoding of ``dev_t``.
"""

from typing import Optional, Iterable
from drgn import Program, Object, cast
from drgn.helpers.linux.list import klist_for_each_entry

__all__ = (
    "MAJOR",
    "MINOR",
    "MKDEV",
)


# This hasn't changed since at least v2.6.
_MINORBITS = 20
_MINORMASK = (1 << _MINORBITS) - 1


def MAJOR(dev):
    """
    .. c:function:: unsigned int MAJOR(dev_t dev)

    Return the major ID of a kernel ``dev_t``.
    """
    major = dev >> _MINORBITS
    if isinstance(major, Object):
        return cast("unsigned int", major)
    return major


def MINOR(dev):
    """
    .. c:function:: unsigned int MINOR(dev_t dev)

    Return the minor ID of a kernel ``dev_t``.
    """
    minor = dev & _MINORMASK
    if isinstance(minor, Object):
        return cast("unsigned int", minor)
    return minor


def MKDEV(major, minor):
    """
    .. c:function:: dev_t MKDEV(unsigned int major, unsigned int minor)

    Return a kernel ``dev_t`` from the major and minor IDs.
    """
    dev = (major << _MINORBITS) | minor
    if isinstance(dev, Object):
        return cast("dev_t", dev)
    return dev

def for_each_class_device(prog: Program, class_struct: Object,
                          subtype: Optional[Object] = None) -> Iterable[Object]:
    try:
        class_in_private = prog.cache["knode_class_in_device_private"]
    except KeyError:
        # We need a proper has_member(), but this is fine for now.
        class_in_private = any(
            member.name == "knode_class"
            for member in prog.type("struct device_private").members
        )
        prog.cache["knode_class_in_device_private"] = class_in_private
    devices = class_struct.p.klist_devices.address_of_()

    if class_in_private:
        container_type = "struct device_private"
    else:
        container_type = "struct device"

    for device in klist_for_each_entry(container_type, devices, "knode_class"):
        if class_in_private:
            device = device.dev

        if subtype is None or subtype == device.type:
            yield device
