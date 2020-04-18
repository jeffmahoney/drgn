# Copyright (c) Facebook, Inc. and its affiliates.
# SPDX-License-Identifier: GPL-3.0+

"""
CPU Scheduler
-------------

The ``drgn.helpers.linux.sched`` module provides helpers for working with the
Linux CPU scheduler.
"""

from _drgn import _linux_helper_task_state_to_char

__all__ = ("task_state_to_char",)

def task_thread_info(task):
    thread_info_p = prog['struct thread_info'].pointer()
    return task.stack.cast(thread_info_p)

def task_last_cpu(task):
    try:
        cpu = task.cpu
    except AttributeError:
        cpu = task_thread_info(task).cpu

    return int(cpu)

def task_name(task):
    return str(task.comm.string_(), 'utf-8', 'backslashreplace')

# < v2.4.0..2.6.12
def _get__rss_ulong(task):
    return int(task.mm.rss)

# 2.6.12..2.6.34
# >= 2.6.12: _rss and _anon_rss, type unsigned long
# >= 2.6.16: _file_rss and _anon_rss, type unsigned long or
#            atomic_long_t depending on configuration
def _get__rss_anon_ulong(task):
    return int(task.mm._rss) + int(task.mm._anon_rss)

def _get__rss_anon_atomic(task):
    return int(task.mm._rss.counter) + int(task.mm._anon_rss.counter)

def _get__file_rss__anon_rss_ulong(task):
    return int(task.mm._file_rss) + int(task.mm._anon_rss)

def _get__file_rss__anon_rss_atomic(task):
    return int(task.mm._file_rss.counter) + int(task.mm._anon_rss.counter)

def _get_rss_stat_field_ulong(task):
    stat = task.mm.rss_stat.count
    rss = 0
    for i in range(stat.type_.length):
        rss += int(stat[i])
    return rss

_alternate_task_rss = [
    _get_rss_stat_field_ulong,
    _get__file_rss__anon_rss_atomic,
    _get__file_rss__anon_rss_ulong,
    _get__rss_anon_atomic,
    _get__rss_anon_ulong,
    _get__rss_ulong
]

# Starting with v3.0, we have sanity:
# Fields in a separate structure, always atomic_long_t
# v2.6.34..v3.0 uses rss_stat but the fields can be
def _get_rss_stat_field_atomic(task):
    stat = task.mm.rss_stat.count
    rss = 0
    for i in range(stat.type_.length):
        rss += int(stat[i].counter)
    return rss

_get_task_rss = _get_rss_stat_field_atomic

def _discover_task_rss(task):
    rss = 0
    for func in _alternate_task_rss:
        try:
            rss = func(task)
            _get_task_rss = func
            return rss
        except AttributeError:
            pass

    raise NotImplementedError("No task_rss helper found for this kernel version")

def task_rss(task):
    try:
        return _get_task_rss(task)
    except AttributeError:
        return _discover_task_rss(task)

def task_is_kthread(task):
    if task.pid == 0:
        return True

    state = _linux_helper_task_state_to_char(task)
    if state == 'Z' or state == 'X':
        return False

    if int(task.mm) == 0:
        return True

    if task.mm.address_ == task.prog_['init_mm'].address_:
        return True

    return False

def task_is_thread_group_leader(task):
    return int(task.exit_signal) >= 0

def task_state_to_char(task):
    """
    .. c:function char task_state_to_char(struct task_struct *task)

    Get the state of the task as a character (e.g., ``'R'`` for running). See
    `ps(1)
    <http://man7.org/linux/man-pages/man1/ps.1.html#PROCESS_STATE_CODES>`_ for
    a description of the process state codes.

    :rtype: str
    """
    return _linux_helper_task_state_to_char(task)
