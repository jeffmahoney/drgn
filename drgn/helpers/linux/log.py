# -*- coding: utf-8 -*-
# vim:set shiftwidth=4 softtabstop=4 expandtab textwidth=79:

import drgn
from drgn import cast

__all__ = [
    'open_kernel_log'
    ]

class KernelLogMessage:
    def __init__(self, prog, msg):
        self.msg = msg

    def __str__(self):
        return self.msg

class StructuredKernelLogMessage(KernelLogMessage):
    def __init__(self, prog, msg):
        super(StructuredKernelLogMessage, self).__init__(prog, msg)
        self._next = 0
        self.timestamp = 0
        self.level = 0
        self.metadata = None

    def __str__(self):
        return self.msg

class StructuredKernelLog:
    def __init__(self, prog):
        self._prog = prog
        self._log_buf = prog['log_buf']
        self._log_first_idx = prog['log_first_idx']
        self._log_next_idx = prog['log_next_idx']
        self._clear_seq = prog['clear_seq']
        self._log_first_seq = prog['log_first_seq']
        self._log_next_seq = prog['log_next_seq']

        self._printk_log_size = drgn.sizeof(prog.type('struct printk_log'))
        self._printk_log_p_type = prog.type('struct printk_log *')
        self._char_p_type = prog.type('char *')

        self.first_idx = self._log_first_idx
        self.first_seq = self._clear_seq
        if self._clear_seq < self._log_first_seq:
            self.first_seq = self._log_first_seq
        self.last_seq = self._log_next_seq

    def __iter__(self):
        return StructuredLogIterator(self)

    def message_at_index(self, index):
        printk_log = cast(self._printk_log_p_type, self._log_buf + index)
        rawtext = cast(self._char_p_type, printk_log) + self._printk_log_size
        text = str(rawtext.string_()[0:printk_log.text_len],
                   'utf-8', 'backslashreplace')

        msg = StructuredKernelLogMessage(self._prog, text)

        if printk_log.len:
            msg.next_idx = index + printk_log.len

        msg.timestamp = int(printk_log.ts_nsec)
        msg.level = int(printk_log.level)

        if int(printk_log.dict_len):
            d = rawtext + printk_log.text_len
            msg.metadata = str(d.string_(), 'utf-8', 'backslashreplace')

        return msg

    def __str__(self):
        out = ""
        for msg in self:
            out += msg.msg + "\n"
        return out
            

class StructuredLogIterator:
    def __init__(self, log):
        self.log = log
        self.idx = log.first_idx
        self.seq = log.first_seq

    def __next__(self):
        if self.seq >= self.log.last_seq:
            raise StopIteration()

        msg = self.log.message_at_index(self.idx)
        self.seq += 1
        self.idx = msg.next_idx

        return msg

def open_kernel_log(prog):
    try:
        log = StructuredKernelLog(prog)
    except KeyError:
        log = UnstructuredKernelLog(prog)

    return log
