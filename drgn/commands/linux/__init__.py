# -*- coding: utf-8 -*-
# vim:set shiftwidth=4 softtabstop=4 expandtab textwidth=79:

import importlib
import pkgutil

__all__ = []

__all__ = []
for _module_info in pkgutil.iter_modules(__path__, prefix=__name__ + "."):
    _submodule = importlib.import_module(_module_info.name)
    __all__.extend(_submodule.__all__)
    for _name in _submodule.__all__:
        cmdcls = getattr(_submodule, _name)
        cmdcls()
