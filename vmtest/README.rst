drgn VM Testing
===============

drgn has a significant amount of code (both core and in helpers) which is
dependent on the Linux kernel version. This code is tested on multiple Linux
kernel versions in a virtual machine. These tests can be run on all supported
kernels with ``python3 setup.py test -K``. This requires QEMU and zstd to be
installed.

Tests can also be run on specific kernels with ``-k``. This takes a
comma-separated list of kernels which are either a wildcard pattern (e.g.,
``5.6.*``) that matches a kernel release hosted on Dropbox (see below) or a
kernel build directory path starting with ``.`` or ``/``.

Architecture
------------

The goal of vmtest is to run tests in the same userspace environment as the
host, but with a different kernel. The host runs the virtual machine with `QEMU
<https://www.qemu.org/>`_ (see the `vmtest.vm <vm.py>`_ module).

The guest mounts the host's root filesystem as its own root filesystem via
`VirtFS <https://www.linux-kvm.org/page/VirtFS>`_. It is mounted read-only for
safety. To support modifications, the guest uses `OverlayFS
<https://www.kernel.org/doc/Documentation/filesystems/overlayfs.txt>`_ to
overlay a read-write tmpfs over the VirtFS root.

The guest runs a `special init process <init.c>`_ which sets up the system and
filesystem hierarchy (including creating the appropriate link to vmlinux) and
communicates with the host over `virtio-serial
<https://fedoraproject.org/wiki/Features/VirtioSerial>`_. The protocol is
essentially that the host sends the arguments to `execvpe(3)
<http://man7.org/linux/man-pages/man3/exec.3.html>`_ and the guest replies with
the `wait(2) <http://www.man7.org/linux/man-pages/man2/wait.2.html>`_ status.

This infrastructure is all fairly generic. The drgn-specific parts are:

1. The kernel builds. The `kernel configuration <config>`_ includes everything
   required to run drgn and the Linux kernel helper tests. These builds are
   hosted on `Dropbox
   <https://www.dropbox.com/sh/2mcf2xvg319qdaw/AAChpI5DJZX2VwlCgPFDdaZHa?dl=0>`_.
   They are managed via the Dropbox API by the `vmtest.manage <manage.py>`_ CLI
   and downloaded by the `vmtest.resolver <resolver.py>`_ module.
2. The test command itself. This is just some ``setup.py`` glue and the proper
   invocation of the Python `unittest command line interface
   <https://docs.python.org/3/library/unittest.html#test-discovery>`_.

The ``vmtest.vm`` and ``vmtest.resolver`` modules also have CLIs for testing
purposes. These are subject to change.
