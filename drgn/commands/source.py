# -*- coding: utf-8 -*-
# vim:set shiftwidth=4 softtabstop=4 expandtab textwidth=79:
"""
SUMMARY
-------

Load and execute a separate python script from a file

::

  source <path>

DESCRIPTION
-----------

"""

import argparse

import drgn
from drgn.commands import Command, CommandError, ArgumentParser

__all__ = [
    "SourceCommand",
]

class SourceCommand(Command):
    """ source command"""

    def __init__(self) -> None:
        parser = ArgumentParser(prog="source")
        parser.add_argument('path', nargs='+')
        parser.add_argument('args', nargs=argparse.REMAINDER)
        super().__init__('source', parser)

    def execute(self, prog, args: argparse.Namespace) -> None:
        globals()['prog'] = prog
        return drgn.execscript(args.path[0], args.args)
