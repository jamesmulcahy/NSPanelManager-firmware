"""
custom_pcb (pioarduino's builder): keep each "-include <header>" pair together.

ESP-Hosted 3.x adds "SHELL:-include eh_host_port_master_config.h" as an interface compile option, so
it is among the app's flags (src/ and lib/). pioarduino's espidf.py sorts the app's flags
(get_app_flags(), "CMake randomly populates build flags") before env.MergeFlags(), which takes the
"-include" away from its header: g++ then sees the header as a second input file ("cannot specify
'-o' with '-c' ... with multiple files"). This wraps MergeFlags to pair them up again.

The components themselves (ESP-Hosted's eh_host_* libraries) lose it another way: in CMake's code
model "-include" and the header are two fragments, and prepare_build_envs() parses each on its own
(parse_flag_extended() -> env.ParseFlags()), which drops the header. Those files then compile
without eh_host_port_master_config.h, every EH_HOST_FEAT_* gate reads 0 and the features compile
to nothing (link: "undefined reference to eh_host_sys_get_mac"). This wraps ParseFlags to join a
lone "-include" with the fragment after it.
"""

import glob
import os

from SCons.Environment import Base as SConsEnvironment

Import("env")  # noqa: F821  (SCons)

_FLAG_KEYS = ("CCFLAGS", "CFLAGS", "CXXFLAGS", "ASPPFLAGS", "ASFLAGS")


def _repair(flags):
    if not isinstance(flags, (list, tuple)) or "-include" not in flags:
        return flags
    # The headers that lost their "-include": bare words naming a header.
    headers = [f for f in flags if isinstance(f, str) and not f.startswith("-") and f.endswith(".h")]
    lone = sum(1 for f in flags if f == "-include")
    if lone != len(headers):
        print("fix_forced_includes: %d -include, %d headers in %r; left as they are" % (lone, len(headers), flags))
        return flags
    rest = [f for f in flags if f != "-include" and f not in headers]
    return rest + [("-include", h) for h in headers]


def _header_dirs(args):
    """The directories in managed_components/ holding the forced headers: the include directory comes
    from an INTERFACE library (eh_host_config), which the builder does not give the app."""
    headers = set()
    for key in _FLAG_KEYS:
        for f in args.get(key) or []:
            if isinstance(f, tuple) and len(f) == 2 and f[0] == "-include":
                headers.add(f[1])
    dirs = []
    for header in sorted(headers):
        found = sorted(glob.glob(os.path.join(env.subst("$PROJECT_DIR"), "managed_components", "**", header), recursive=True))  # noqa: F821
        if found:
            dirs.append(os.path.dirname(found[0]))
        else:
            print("fix_forced_includes: %s not found in managed_components" % header)
    return dirs


def _merge_flags_paired(self, args, *rest, **kwargs):
    if isinstance(args, dict):
        args = {k: (_repair(v) if k in _FLAG_KEYS else v) for k, v in args.items()}
        dirs = _header_dirs(args)
        if dirs:
            self.AppendUnique(CPPPATH=dirs)
    # Unbound: AddMethod() copies this to every Clone() (the bootloader's, the libraries'), and each
    # must merge into itself.
    return SConsEnvironment.MergeFlags(self, args, *rest, **kwargs)


# A ParseFlags() call that got "-include" alone; the next call's header goes with it. The fragments
# of one compile group are parsed one after another, so one pending flag is enough.
_include_pending = [False]


def _parse_flags_paired(self, *flags, **kwargs):
    if len(flags) == 1 and isinstance(flags[0], str):
        flag = flags[0].strip()
        if flag == "-include":
            _include_pending[0] = True
            return SConsEnvironment.ParseFlags(self)
        if _include_pending[0]:
            _include_pending[0] = False
            if not flag.startswith("-"):
                # As a bare name, for the include path to find (ParseFlags would make it a path
                # relative to the builder's directory).
                parsed = SConsEnvironment.ParseFlags(self)
                parsed["CCFLAGS"].append(("-include", flag))
                return parsed
    _include_pending[0] = False
    return SConsEnvironment.ParseFlags(self, *flags, **kwargs)


env.AddMethod(_merge_flags_paired, "MergeFlags")  # noqa: F821
env.AddMethod(_parse_flags_paired, "ParseFlags")  # noqa: F821
