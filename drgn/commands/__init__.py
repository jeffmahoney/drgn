# -*- coding: utf-8 -*-
# vim:set shiftwidth=4 softtabstop=4 expandtab textwidth=79:

import argparse
import drgn
import shlex
import importlib

from typing import Any, Optional, List
import os

__all__ = [
    'lookup_command',
    'run_command',
    'source',
    'help',
    ]

_commands = {}

class CommandError(RuntimeError):
    """An error occured while executing this command"""

class CommandLineError(RuntimeError):
    """An error occured while handling the command line for this command"""

class NoSuchCommandError(RuntimeError):
    pass

class ArgumentParser(argparse.ArgumentParser):
    """
    A simple extension to :class:`argparse.ArgumentParser` that:

    - Requires a command name be set
    - Loads help text automatically from files
    - Handles errors by raising :obj:`.CommandLineError`

    """
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)

        if not self.prog:
            raise CommandError("Cannot build command with no name")

    def error(self, message: str) -> Any:
        """
        An error callback that raises the :obj:`CommandLineError` exception.
        """
        raise CommandLineError(message)

    def format_help(self) -> str:
        """
        A help formatter that loads the parsed rST documentation from disk
        or returns the generic help text otherwise.
        """
        try:
            path = os.path.join(os.environ['CRASH_PYTHON_HELP'], 'commands',
                                f"{self.prog}.txt")
            f = open(path)
            helptext = f.read()
            f.close()
        except (KeyError, FileNotFoundError):
            helptext = "Could not locate help file.\n"
            helptext += "Generic help text follows.\n\n"
            helptext += super().format_help()

        return helptext

class Command:
    def __init__(self, name: str,
                 parser: Optional[ArgumentParser] = None) -> None:
        self.name = name
        if parser is None:
            parser = ArgumentParser(prog=self.name)
        elif not isinstance(parser, ArgumentParser):
            raise TypeError("parser must be derived from command.ArgumentParser")

        self._parser = parser
        self.add_name(self.name)

    # This can be called multiple times with the same command to create
    # aliases.
    def add_name(self, name: str) -> None:
        _commands[name] = self

    def format_help(self) -> str:
        return self._parser.format_help()

    def _commands(self):
        return _commands

    # pylint: disable=unused-argument
    def invoke_uncaught(self, prog, argv: List[str]) -> None:
        """
        Invokes the command directly and does not catch exceptions.

        This is used mainly for unit testing to ensure proper exceptions
        are raised.

        Unless you are doing something special, see :meth:`execute` instead.

        Args:
            argstr: The command arguments
        """
        args = self._parser.parse_args(argv)
        self.execute(prog, args)

    def invoke(self, prog, argv: List[str]) -> None:
        """
        Invokes the command directly and translates exceptions.

        This method is called by ``gdb`` to implement the command.

        It translates the :class:`.CommandError` and
        :class:`.CommandLineError`, exceptions into readable
        error messages.

        Unless you are doing something special, see :meth:`execute` instead.

        Args:
            argstr: The command arguments
        """
        try:
            self.invoke_uncaught(prog, argv)
        except CommandError as e:
            print(f"{self.name}: {str(e)}")
        except CommandLineError as e:
            print(f"{self.name}: {str(e)}")
            self._parser.print_usage()
        except (SystemExit, KeyboardInterrupt):
            pass

    def execute(self, prog, args: argparse.Namespace) -> None:
        """
        This method implements the command functionality.

        Each command has a derived class associated with it that,
        minimally, implements this method.

        Args:
            args: The arguments to this command already parsed by the
                commmand's parser.
        """
        raise NotImplementedError("Command should not be called directly")

def lookup_command(name) -> Optional[Command]:
    try:
        return _commands[name]
    except KeyError:
        return None

def run_command(prog, cmdstr: str):
    try:
        cmdline = shlex.split(cmdstr)
        if cmdline:
            cmd = lookup_command(cmdline[0])
            args = cmdline[1:]
    except ValueError:
        cmd = None

    if cmd is None:
        raise NoSuchCommandError(f"No command for input: {cmdstr}")

    return cmd.invoke(prog, args)

base_commands = [ 'help', 'source' ]

for cmdname in base_commands:
    module = importlib.import_module(f"drgn.commands.{cmdname}")
    for name in module.__dict__["__all__"]:
        cmdcls = getattr(module, name)
        cmdcls()
