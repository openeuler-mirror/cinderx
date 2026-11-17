"""Model the registry operations exercised by the same-name field test."""


class Parameter:
    pass


class Module:
    def __init__(self):
        object.__setattr__(self, "_parameters", {})
        object.__setattr__(self, "_buffers", {})
        object.__setattr__(self, "_modules", {})

    def register_buffer(self, name, value):
        self._buffers[name] = value

    def add_module(self, name, value):
        self._modules[name] = value

    def __setattr__(self, name, value):
        if isinstance(value, Parameter):
            self._buffers.pop(name, None)
            self._parameters[name] = value
        else:
            object.__setattr__(self, name, value)

    def __delattr__(self, name):
        for registry in (self._parameters, self._buffers, self._modules):
            if name in registry:
                del registry[name]
                return
        object.__delattr__(self, name)

    # The bytecode of this handler is part of the registration contract.
    def __getattr__(self, name):
        if "_parameters" in self.__dict__:
            _parameters = self.__dict__["_parameters"]
            if name in _parameters:
                return _parameters[name]
        if "_buffers" in self.__dict__:
            _buffers = self.__dict__["_buffers"]
            if name in _buffers:
                return _buffers[name]
        if "_modules" in self.__dict__:
            modules = self.__dict__["_modules"]
            if name in modules:
                return modules[name]
        raise AttributeError(
            f"'{type(self).__name__}' object has no attribute '{name}'"
        )
