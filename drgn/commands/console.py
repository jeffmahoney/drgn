# Copyright 2018-2019 - Omar Sandoval
# SPDX-License-Identifier: GPL-3.0+

import code
import sys
import re

class CommandConsole(code.InteractiveConsole):
    def __init__(self, local):
        prog = local['prog']
        super(CommandConsole, self).__init__(local)
        self.more = 0

        import drgn.commands

        if prog.flags & drgn.ProgramFlags.IS_LINUX_KERNEL:
            import drgn.commands.linux

        self.in_command_mode = True
        self.update_prompt()
        self.exit_pending = False

    def update_prompt(self):
        if self.in_command_mode:
            sys.ps1 = 'cmd>>> '
        else:
            sys.ps1 = '>>> '

    def raw_input(self, prompt=""):
        tty = sys.stdin.isatty()
        if not tty:
            prompt = ""
        try:
            line = super(CommandConsole, self).raw_input(prompt)
        except EOFError as e:
            if not tty:
                sys.exit()
            if self.nested_state() and not self.exit_pending:
                self.exit_nested_state()
                self.exit_pending = True
                return "\n"
            raise e
        return line

    def enter_nested_state(self):
        self.in_command_mode = False
        self.update_prompt()

    def exit_nested_state(self):
        self.in_command_mode = True
        self.update_prompt()

    def nested_state(self):
        return not self.in_command_mode

    def runcode(self, code):
        try:
            exec(code, self.locals)
        except drgn.commands.NoSuchCommandError as e:
            print(e)
        except SystemExit:
            raise
        except:
            self.showtraceback()

    def push(self, line):
        self.exit_pending = False
        if self.more:
            self.more = super(CommandConsole, self).push(line)
            return self.more

        # Strip comments
        m = re.match("([^#]+)(#.*)?", line)
        if m:
            cmdline = m.group(1)
        else:
            cmdline = line
        cmdline = cmdline.strip()

        if self.nested_state():
            if cmdline == "end":
                self.exit_nested_state()
                return 0
        else:
            if cmdline == "python":
                self.enter_nested_state()
                return 0

        if self.in_command_mode and cmdline:
            line = f"drgn.commands.run_command(prog, \"{cmdline}\")"

        try:
            self.more = super(CommandConsole, self).push(line)
        except drgn.commands.NoSuchCommandError as e:
            print(e)

        return self.more


