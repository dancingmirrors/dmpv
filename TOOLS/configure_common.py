#!/usr/bin/env python3
import atexit
import os
import shutil
import subprocess
import sys
import tempfile

NoneType = type(None)
function = type(lambda: 0)

programs_info = [
    # env. name     default
    ("CC",          "cc"),
    ("CLANG",       "clang"),
    ("GCC",         "gcc"),
    ("PKG_CONFIG",  "pkg-config"),
    ("WAYSCAN",     "wayland-scanner"),
    ("GIT",         "git"),
    ("NINJA",       "ninja"),
]

install_paths_info = [
    # env/opt       default
    ("PREFIX",      "/usr/local"),
    ("CONFDIR",     "$(PREFIX)/etc"),
    ("CONFLOADDIR", "$(CONFDIR)"),
]

# for help output only; code grabs them manually
other_env_vars = [
    # env           # help text
    ("CFLAGS",      "User C compiler flags to append."),
    ("CPPFLAGS",    "Also treated as C compiler flags."),
    ("LDFLAGS",     "C compiler flags for link command."),
    ("CCACHE",      "Set to 'no' to disable automatic ccache detection."),
    ("TARGET",      "Prefix for default build tools (for cross compilation)"),
    ("CROSS_COMPILE", "Same as TARGET."),
]

class _G:
    help_mode = False   # set if --help is specified on the command line

    log_file = None     # opened log file

    temp_path = None    # set to a private, writable temporary directory
    build_dir = None
    root_dir = None
    out_of_tree = False

    install_paths = {}  # var name to path, see install_paths_info

    programs = {}       # key is symbolic name, like CC, value is string of
                        # executable name - only set if check_program was called

    exe_format = "elf"

    cflags = []
    ldflags = []

    config_h = ""       # new contents of config.h (written at the end)
    config_mak = ""     # new contents of config.mak (written at the end)

    sources = []

    state_stack = []

    feature_opts = {}   # keyed by option name, values are:
                        #   "yes": force enable, like --enable-<feature>
                        #   "no": force disable, like: --disable-<feature>
                        #   "auto": force auto detection, like --with-<feature>=auto
                        #   "default": default (same as option not given)

    dep_enabled = {}    # keyed by dependency identifier; value is a bool
                        # missing key means the check was not run yet


# Convert a string to a C string literal. Adds the required "".
def _c_quote_string(s):
    s = s.replace("\\", "\\\\")
    s = s.replace("\"", "\\\"")
    return "\"%s\"" % s

# Convert a string to a make variable. Escaping is annoying: sometimes, you add
# e..g arbitrary paths (=> everything escaped), but sometimes you want to keep
# make variable use like $(...) unescaped.
def _c_quote_makefile_var(s):
    s = s.replace("\\", "\\\\")
    s = s.replace("\"", "\\\"")
    return s

def die(msg):
    sys.stderr.write("Fatal error: %s\n" % msg)
    sys.stderr.write("Not updating build files.\n")
    if _G.log_file:
        _G.log_file.write("--- Stopping due to error: %s\n" % msg)
    sys.exit(1)

# To be called before any user checks are performed.
def begin():
    _G.root_dir = "."
    _G.build_dir = "build"

    projname = os.environ.get("PROJNAME")
    if not projname:
        try:
            makefile_path = os.path.join(_G.root_dir, "Makefile")
            if os.path.exists(makefile_path):
                with open(makefile_path, "r") as mf:
                    for ln in mf:
                        line = ln.strip()
                        if line.startswith("PROJNAME"):
                            parts = line.split("=", 1)
                            if len(parts) == 2:
                                projname = parts[1].strip()
                                if projname.startswith('"') and projname.endswith('"'):
                                    projname = projname[1:-1]
                                projname = projname.split()[0]
                                break
        except Exception:
            projname = None
    if not projname:
        try:
            projname = os.path.basename(os.path.realpath(_G.root_dir)) or "dmpv"
        except Exception:
            projname = "dmpv"
    _G.install_paths["PROJNAME"] = projname

    for var, val in install_paths_info:
        _G.install_paths[var] = val

    for arg in sys.argv[1:]:
        if arg.startswith("-"):
            name = arg[1:]
            if name.startswith("-"):
                name = name[1:]
            opt = name.split("=", 1)
            name = opt[0]
            val = opt[1] if len(opt) > 1 else ""
            def noval():
                if val:
                    die("Option --%s does not take a value." % name)
            if name == "help":
                noval()
                _G.help_mode = True
                continue
            elif name.startswith("enable-"):
                noval()
                _G.feature_opts[name[7:]] = "yes"
                continue
            elif name.startswith("disable-"):
                noval()
                _G.feature_opts[name[8:]] = "no"
                continue
            elif name.startswith("with-"):
                if val not in ["yes", "no", "auto", "default"]:
                    die("Option --%s requires 'yes', 'no', 'auto', or 'default'."
                        % name)
                _G.feature_opts[name[5:]] = val
                continue
            uname = name.upper()
            setval = None
            if uname in _G.install_paths:
                def set_install_path(name, val):
                    _G.install_paths[name] = val
                setval = set_install_path
            elif uname == "BUILDDIR":
                def set_build_path(name, val):
                    _G.build_dir = val
                setval = set_build_path
            if not setval:
                die("Unknown option: %s" % arg)
            if not val:
                die("Option --%s requires a value." % name)
            setval(uname, val)
            continue

    if _G.help_mode:
        print("Environment variables controlling choice of build tools:")
        for name, default in programs_info:
            print("  %-30s %s" % (name, default))

        print("")
        print("Environment variables/options controlling install paths:")
        for name, default in install_paths_info:
            print("  %-30s '%s' (also --%s)" % (name, default, name.lower()))

        print("")
        print("Other environment variables:")
        for name, help in other_env_vars:
            print("  %-30s %s" % (name, help))
        print("In addition, pkg-config queries PKG_CONFIG_PATH.")
        print("")
        print("General build options:")
        print("  %-30s %s" % ("--builddir=PATH", "Build directory (default: build)"))
        print("")
        print("Specific build configuration:")
        # check() invocations will print the options they understand.
        return

    _G.temp_path = tempfile.mkdtemp(prefix = "dmpv-configure-")
    def _cleanup():
        shutil.rmtree(_G.temp_path)
    atexit.register(_cleanup)

    # (os.path.samefile() is "UNIX only")
    if os.path.realpath(sys.path[0]) != os.path.realpath(os.getcwd()):
        print("This looks like an out of tree build.")
        print("This doesn't actually work.")
        # Keep the build dir; this makes it less likely to accidentally trash
        # an existing dir, especially if dist-clean (wipes build dir) is used.
        # Also, this will work even if the same-directory check above was wrong.
        _G.build_dir = os.path.join(os.getcwd(), _G.build_dir)
        _G.root_dir = sys.path[0]
        _G.out_of_tree = True

    # Wipe the build directory completely to avoid leftover files causing issues
    if os.path.exists(_G.build_dir):
        shutil.rmtree(_G.build_dir)
    os.makedirs(_G.build_dir, exist_ok = True)
    _G.log_file = open(os.path.join(_G.build_dir, "config.log"), "w")

    _G.config_h += "// Generated by configure.\n" + \
                   "#pragma once\n\n"


# Check whether the first argument is the same type of any in the following
# arguments. This _always_ returns val, but throws an exception if type checking
# fails.
# This is not very Pythonic, but I'm trying to prevent bugs, so bugger off.
def typecheck(val, *types):
    vt = type(val)
    for t in types:
        if vt == t:
            return val
    raise Exception("Value '%s' of type %s not any of %s" % (val, type(val), types))

# If val is None, return []
# If val is a list, return val.
# Otherwise, return [val]
def normalize_list_arg(val):
    if val is None:
        return []
    if type(val) == list:
        return val
    return [val]

def push_build_flags():
    _G.state_stack.append(
        (_G.cflags[:], _G.ldflags[:], _G.config_h, _G.config_mak,
         _G.programs.copy()))

def pop_build_flags_discard():
    top = _G.state_stack[-1]
    _G.state_stack = _G.state_stack[:-1]

    (_G.cflags[:], _G.ldflags[:], _G.config_h, _G.config_mak,
     _G.programs) = top

def pop_build_flags_merge():
    top = _G.state_stack[-1]
    _G.state_stack = _G.state_stack[:-1]

# Return build dir.
def get_build_dir():
    assert _G.build_dir is not None # too early?
    return _G.build_dir

# Root directory, i.e. top level source directory, or where configure/Makefile
# are located.
def get_root_dir():
    assert _G.root_dir is not None # too early?
    return _G.root_dir

# Set which type of executable format the target uses.
# Used for conventions which refuse to abstract properly.
def set_exe_format(fmt):
    assert fmt in ["elf", "pe", "macho"]
    _G.exe_format = fmt

# A check is a check, dependency, or anything else that adds source files,
# preprocessor symbols, libraries, include paths, or simply serves as
# dependency check for other checks.
# Always call this function with named arguments.
# Arguments:
#   name: String or None. Symbolic name of the check. The name can be used as
#         dependency identifier by other checks. This is the first argument, and
#         usually passed directly, instead of as named argument.
#         If this starts with a "-" flag, options with names derived from this
#         are generated:
#           --enable-$option
#           --disable-$option
#           --with-$option=<yes|no|auto|default>
#         Where "$option" is the name without flag characters, and occurrences
#         of "_" are replaced with "-".
#         If this ends with a "*" flag, the result of this check is emitted as
#         preprocessor symbol to config.h. It will have the name "HAVE_$DEF",
#         and will be either set to 0 (check failed) or 1 (check succeeded),
#         and $DEF is the name without flag characters and all uppercase.
#   desc: String or None. If specified, "Checking for <desc>..." is printed
#         while running configure. If not specified, desc is auto-generated from
#         the name.
#   default: Boolean or None. If True or None, the check is soft-enabled (that
#            means it can still be disabled by options, dependency checks, or
#            the check function). If False, the check is disabled by default,
#            but can be enabled by an option.
#   deps, deps_any, deps_neg: String, array of strings, or None. If a check is
#       enabled by default/command line options, these checks are performed in
#       the following order: deps_neg, deps_any, deps
#       deps requires all dependencies in the list to be enabled.
#       deps_any requires 1 or more dependencies to be enabled.
#       deps_neg requires that all dependencies are disabled.
#   fn: Function or None. The function is run after dependency checks. If it
#       returns True, the check is enabled, if it's False, it will be disabled.
#       Typically, your function for example check for the existence of
#       libraries, and add them to the final list of CFLAGS/LDFLAGS.
#       None behaves like "lambda: True".
#       Note that this needs to be a function. If not, it'd be run before the
#       check() function is even called. That would mean the function runs even
#       if the check was disabled, and could add unneeded things to CFLAGS.
#       If this function returns False, all added build flags are removed again,
#       which makes it easy to compose checks.
#   sources: String, Array of Strings, or None.
#            If the check is enabled, add these sources to the build.
#            Duplicate sources are removed at end of configuration.
#   required: String or None. If this is a string, the check is required, and
#             if this is not enabled, the string is printed as error message.
def check(name = None, option = None, desc = None, deps = None, deps_any = None,
          deps_neg = None, sources = None, fn = None, required = None,
          default = None):

    deps = normalize_list_arg(deps)
    deps_any = normalize_list_arg(deps_any)
    deps_neg = normalize_list_arg(deps_neg)
    sources = normalize_list_arg(sources)

    typecheck(name, str, NoneType)
    typecheck(option, str, NoneType)
    typecheck(desc, str, NoneType)
    typecheck(deps, NoneType, list)
    typecheck(deps_any, NoneType, list)
    typecheck(deps_neg, NoneType, list)
    typecheck(sources, NoneType, list)
    typecheck(fn, NoneType, function)
    typecheck(required, str, NoneType)
    typecheck(default, bool, NoneType)

    option_name = None
    define_name = None
    if name is not None:
        opt_flag = name.startswith("-")
        if opt_flag:
            name = name[1:]
        def_flag = name.endswith("*")
        if def_flag:
            name = name[:-1]
        if opt_flag:
            option_name = name.replace("_", "-")
        if def_flag:
            define_name = "HAVE_" + name.replace("-", "_").upper()

    if desc is None and name is not None:
        desc = name

    if _G.help_mode:
        if not option_name:
            return

        defaction = "enable"
        if required is not None:
            # If they are required, but also have option set, these are just
            # "strongly required" options.
            defaction = "enable"
        elif default == False:
            defaction = "disable"
        elif deps or deps_any or deps_neg or fn:
            defaction = "autodetect"
        act = "enable" if defaction == "disable" else "disable"
        opt = "--%s-%s" % (act, option_name)
        print("  %-30s %s %s [%s]" % (opt, act, desc, defaction))
        return

    _G.log_file.write("\n--- Test: %s\n" % (name if name else "(unnamed)"))

    if desc:
        sys.stdout.write("Checking for %s... " % desc)
    outcome = "yes"

    force_opt = required is not None
    use_dep = True if default is None else default

    # Option handling.
    if option_name:
        # (The option gets removed, so we can determine whether all options were
        # applied in the end.)
        val = _G.feature_opts.pop(option_name, "default")
        if val == "yes":
            use_dep = True
            force_opt = True
        elif val == "no":
            use_dep = False
            force_opt = False
        elif val == "auto":
            use_dep = True
        elif val == "default":
            pass
        else:
            assert False

    if not use_dep:
        outcome = "disabled"

    # Dependency resolution.
    # But first, check whether all dependency identifiers really exist.
    for d in deps_neg + deps_any + deps:
        dep_enabled(d) # discard result
    if use_dep:
        for d in deps_neg:
            if dep_enabled(d):
                use_dep = False
                outcome = "conflicts with %s" % d
                break
    if use_dep:
        any_found = False
        for d in deps_any:
            if dep_enabled(d):
                any_found = True
                break
        if len(deps_any) > 0 and not any_found:
            use_dep = False
            outcome = "not any of %s found" % (", ".join(deps_any))
    if use_dep:
        for d in deps:
            if not dep_enabled(d):
                use_dep = False
                outcome = "%s not found" % d
                break

    # Running actual checks.
    if use_dep and fn:
        push_build_flags()
        if fn():
            pop_build_flags_merge()
        else:
            pop_build_flags_discard()
            use_dep = False
            outcome = "no"

    # Outcome reporting and terminating if dependency not found.
    if name:
        _G.dep_enabled[name] = use_dep
    if define_name:
        add_config_h_define(define_name, 1 if use_dep else 0)
    if use_dep:
        _G.sources += sources
    if desc:
        sys.stdout.write("%s\n" % outcome)
    _G.log_file.write("--- Outcome: %s (%s=%d)\n" %
                      (outcome, name if name else "(unnamed)", use_dep))

    if required is not None and not use_dep:
        print("Warning: %s" % required)

    if force_opt and not use_dep:
        die("This feature is required.")


# Runs the process like with execv() (just that args[0] is used for both command
# and first arg. passed to the process).
# Returns the process stdout output on success, or None on non-0 exit status.
# In particular, this logs the command and its output/exit status to the log
# file.
def _run_process(args):
    p = subprocess.Popen(args, stdout = subprocess.PIPE,
                         stderr = subprocess.PIPE,
                         stdin = -1)
    (p_out, p_err) = p.communicate()
    # We don't really want this. But Python 3 in particular makes it too much of
    # a PITA to consistently use byte strings, so we need to use "unicode" strings.
    # Yes, a bad program could just blow us up here by outputting invalid UTF-8.
    # Weakly support Python 2 too (GCC outputs UTF-8, which crashes Python 2).
    if type(b"") != str:
        p_out = p_out.decode("utf-8")
        p_err = p_err.decode("utf-8")
    status = p.wait()
    _G.log_file.write("--- Command: %s\n" % " ".join(args))
    if p_out:
        _G.log_file.write("--- stdout:\n%s" % p_out)
    if p_err:
        _G.log_file.write("--- stderr:\n%s" % p_err)
    _G.log_file.write("--- Exit status: %s\n" % status)
    return p_out if status == 0 else None

# Run the C compiler, possibly including linking. Return whether the compiler
# exited with success status (0 exit code) as boolean. What exactly it does
# depends on the arguments. Generally, it constructs a source file and tries
# to compile it. With no arguments, it compiles, but doesn't link, a source
# file that contains a dummy main function.
# Note: these tests are cumulative.
# Arguments:
#   include: String, array of strings, or None. For each string
#            "#include <$value>" is added to the top of the source file.
#   decl: String, array of strings, or None. Added to the top of the source
#         file, global scope, separated by newlines.
#   expr: String or None. Added to the body of the main function. Despite the
#         name, needs to be a full statement, needs to end with ";".
#   defined: String or None. Adds code that fails if "#ifdef $value" fails.
#   flags: String, array of strings, or None. Each string is added to the
#          compiler command line.
#   link: String, array of strings, or None. Each string is added to the
#         compiler command line, and the compiler is made to link (not passing
#         "-c").
#         A value of [] triggers linking without further libraries.
#         A value of None disables the linking step.
#         Also, if the test succeeds, all link strings are added to the LDFLAGS
#         written to config.mak.
def check_cc(include = None, decl = None, expr = None, defined = None,
             flags = None, link = None, language = "c"):
    assert language in ["c"]

    use_linking = link is not None

    contents = ""
    for inc in normalize_list_arg(include):
        contents += "#include <%s>\n" % inc
    for dec in normalize_list_arg(decl):
        contents += "%s\n" % dec
    for define in normalize_list_arg(defined):
        contents += ("#ifndef %s\n" % define) + \
                    "#error failed\n" + \
                    "#endif\n"
    if expr or use_linking:
        contents += "int main(int argc, char **argv) {\n";
        if expr:
            contents += expr + "\n"
        contents += "return 0; }\n"
    source = os.path.join(_G.temp_path, "test." + language)
    _G.log_file.write("--- Test file %s:\n%s" % (source, contents))
    with open(source, "w") as f:
        f.write(contents)

    flags = normalize_list_arg(flags)
    link = normalize_list_arg(link)

    outfile = os.path.join(_G.temp_path, "test")
    # Split CC in case it contains multiple parts (e.g., "ccache gcc")
    cc_parts = get_program("CC").split()
    args = cc_parts + [source]
    args += _G.cflags + flags
    if use_linking:
        args += _G.ldflags + link
        args += ["-o%s" % outfile]
    else:
        args += ["-c", "-o%s.o" % outfile]
    if _run_process(args) is None:
        return False

    _G.cflags += flags
    _G.ldflags += link
    return True

# Run pkg-config with function arguments passed as command arguments. Typically,
# you specify pkg-config version expressions, like "libass >= 0.14". Returns
# success as boolean.
# If this succeeds, the --cflags and --libs are added to CFLAGS and LDFLAGS.
def check_pkg_config(*args):
    args = list(args)
    pkg_config_cmd = [get_program("PKG_CONFIG")]

    cflags = _run_process(pkg_config_cmd + ["--cflags"] + args)
    if cflags is None:
        return False
    ldflags = _run_process(pkg_config_cmd + ["--libs"] + args)
    if ldflags is None:
        return False

    _G.cflags += cflags.split()
    _G.ldflags += ldflags.split()
    return True

def get_pkg_config_variable(arg, varname):
    typecheck(arg, str)
    pkg_config_cmd = [get_program("PKG_CONFIG")]

    res = _run_process(pkg_config_cmd + ["--variable=" + varname] + [arg])
    if res is not None:
        res = res.strip()
    return res

def check_binary_exists(binary_name):
    typecheck(binary_name, str)
    _G.log_file.write("--- Checking for binary '%s' in PATH...\n" % binary_name)

    # Find a binary without executing it.
    binary_path = shutil.which(binary_name)

    if binary_path:
        _G.log_file.write("--- Found '%s' at '%s'\n" % (binary_name, binary_path))
        return True
    else:
        _G.log_file.write("--- Binary '%s' not found in PATH\n" % binary_name)
        return False

# Check for a specific build tool. You pass in a symbolic name (e.g. "CC"),
# which is then resolved to a full name and added as variable to config.mak.
# The function returns a bool for success. You're not supposed to use the
# program from configure; instead you're supposed to have rules in the Makefile
# using the generated variables.
# (Some configure checks use the program directly anyway with get_program().)
def check_program(env_name):
    for name, default in programs_info:
        if name == env_name:
            val = os.environ.get(env_name, None)
            if val is None:
                prefix = os.environ.get("TARGET", None)
                if prefix is None:
                    prefix = os.environ.get("CROSS_COMPILE", "")
                # Dumb hack: default to gcc if a prefix is given, as binutils
                # toolchains generally provide only a -gcc wrapper.
                if prefix and default == "cc":
                    default = "gcc"
                val = prefix + default

            # Auto-detect and enable ccache for C compiler if available
            # Check if ccache should be auto-enabled (unless explicitly disabled or already present)
            if env_name == "CC":
                # Check if user explicitly disabled ccache via CCACHE=no/0/false
                ccache_disabled = os.environ.get("CCACHE", "").lower() in {"no", "0", "false"}
                # Check if CC already contains ccache to avoid double-wrapping
                val_parts = val.split()
                already_has_ccache = val_parts and os.path.basename(val_parts[0]) == "ccache"

                if not ccache_disabled and not already_has_ccache:
                    # Check if ccache is available
                    try:
                        if _run_process(["ccache", "-V"]) is not None:
                            # ccache is available, wrap the compiler with it
                            val = "ccache " + val
                            _G.log_file.write("--- ccache detected, enabling automatic caching\n")
                    except OSError:
                        # ccache not available, continue without it
                        _G.log_file.write("--- ccache not found, proceeding without caching\n")
                elif ccache_disabled:
                    _G.log_file.write("--- ccache explicitly disabled via CCACHE=no\n")
                elif already_has_ccache:
                    _G.log_file.write("--- ccache already present in CC, not adding again\n")

            # Interleave with output. Sort of unkosher, but dare to stop me.
            sys.stdout.write("(%s) " % val)
            _G.log_file.write("--- Trying '%s' for '%s'...\n" % (val, env_name))

            # For CC with ccache, we need to test the actual compiler, not "ccache cc" as a single command
            # Check if first part of CC is ccache (handles both "ccache" and "/usr/bin/ccache")
            val_parts = val.split()
            is_ccache = (env_name == "CC" and len(val_parts) > 1 and
                        os.path.basename(val_parts[0]) == "ccache")
            test_cmd = val_parts if is_ccache else [val]
            try:
                _run_process(test_cmd)
            except OSError as err:
                _G.log_file.write("%s\n" % err)
                return False
            _G.programs[env_name] = val
            add_config_mak_var(env_name, val)
            return True
    assert False, "Unknown program name '%s'" % env_name

# Get the resolved value for a program. Explodes in your face if there wasn't
# a successful and merged check_program() call before.
def get_program(env_name):
    val = _G.programs.get(env_name, None)
    assert val is not None, "Called get_program(%s) without successful check." % env_name
    return val

# Return whether all passed dependency identifiers are fulfilled.
def dep_enabled(*deps):
    for d in deps:
        val = _G.dep_enabled.get(d, None)
        assert val is not None, "Internal error: unknown dependency %s" % d
        if not val:
            return False
    return True

# Add all of the passed strings to CFLAGS.
def add_cflags(*fl):
    _G.cflags += list(fl)

def add_ldflags(*fl):
    _G.ldflags += list(fl)

# Add a preprocessor symbol of the given name to config.h.
# If val is a string, it's quoted as string literal.
# If val is None, it's defined without value.
def add_config_h_define(name, val):
    if type(val) == type("") or type(val) == type(b""):
        val = _c_quote_string(val)
    if val is None:
        val = ""
    _G.config_h += "#define %s %s\n" % (name, val)
    # Also export HAVE_* defines to config.mak so Makefile can read them reliably.
    # Only export simple identifiers (avoid exporting arbitrary strings).
    try:
        import re
        if re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', name) and name.startswith("HAVE_"):
            # val may be a quoted string; normalize to unquoted token or 0/1.
            v = val
            # If it's a quoted string, keep as-is; else if it's numeric or empty, pass through.
            # Remove surrounding quotes if present so config.mak value is a plain token/number.
            if isinstance(v, str) and len(v) >= 2 and v[0] == '"' and v[-1] == '"':
                v = v[1:-1]
            # Write into config.mak using add_config_mak_var so proper escaping is applied.
            add_config_mak_var(name, v)
    except Exception:
        # Best-effort: don't let errors here break configure.
        pass

# Add a makefile variable of the given name to config.mak.
# If val is a string, it's quoted as string literal.
def add_config_mak_var(name, val):
    if type(val) == type("") or type(val) == type(b""):
        val = _c_quote_makefile_var(val)
    _G.config_mak += "%s = %s\n" % (name, val)

# Add these source files to the build.
def add_sources(*sources):
    _G.sources += list(sources)

# Get an environment variable and parse it as flags array.
def _get_env_flags(name):
    res = os.environ.get(name, "").split()
    if len(res) == 1 and len(res[0]) == 0:
        res = []
    return res

# Generate build.ninja file for Ninja backend
def _generate_ninja_file(sources, cflags_str, ldflags_str):
    ninja_content = "# Generated by configure.\n\n"

    # Get programs
    cc = _G.programs.get("CC", "cc")
    wayscan = _G.programs.get("WAYSCAN", "wayland-scanner")

    # Resolve build and root directories (use absolute paths for Ninja)
    build_dir = os.path.abspath(_G.build_dir)
    root_dir = os.path.abspath(_G.root_dir)

    # Get wayland proto dir if available
    wl_proto_dir = ""
    import re
    wl_match = re.search(r'^WL_PROTO_DIR\s*=\s*(.+)$', _G.config_mak, re.MULTILINE)
    if wl_match:
        wl_proto_dir = wl_match.group(1).strip()

    # Pre-create all necessary directories to avoid per-file mkdir overhead
    dirs_to_create = set()
    dirs_to_create.add(os.path.join(build_dir, "generated"))
    dirs_to_create.add(os.path.join(build_dir, "generated/etc"))
    dirs_to_create.add(os.path.join(build_dir, "generated/sub"))
    dirs_to_create.add(os.path.join(build_dir, "generated/player"))
    dirs_to_create.add(os.path.join(build_dir, "generated/player/lua"))
    if wl_proto_dir:
        dirs_to_create.add(os.path.join(build_dir, "generated/wayland"))

    # Create directories for object files based on sources
    for src in sources:
        src_path = src.replace("$(BUILD)/", "").replace("$(ROOT)/", "")
        if src_path.endswith(".c") or src_path.endswith(".rc"):
            obj_dir = os.path.dirname(os.path.join(build_dir, src_path))
            dirs_to_create.add(obj_dir)

    # Create all directories upfront
    for d in dirs_to_create:
        os.makedirs(d, exist_ok=True)

    # Define variables
    ninja_content += f"builddir = {build_dir}\n"
    ninja_content += f"root = {root_dir}\n"
    ninja_content += f"cc = {cc}\n"
    ninja_content += f"wayscan = {wayscan}\n"
    ninja_content += f"cflags = {cflags_str}\n"
    ninja_content += f"ldflags = {ldflags_str}\n"
    exesuf = ".exe" if _G.exe_format == "pe" else ""
    ninja_content += f"exesuf = {exesuf}\n"
    if wl_proto_dir:
        ninja_content += f"wl_proto_dir = {wl_proto_dir}\n"
    ninja_content += "\n"

    # Define rules
    ninja_content += "rule cc\n"
    ninja_content += "  command = $cc $cflags -I$root -I$builddir $in -c -o $out -MF $out.d\n"
    ninja_content += "  description = CC $out\n"
    ninja_content += "  depfile = $out.d\n"
    ninja_content += "  deps = gcc\n"
    ninja_content += "\n"

    ninja_content += "rule link\n"
    ninja_content += "  command = $cc @$out.rsp $ldflags -o $out\n"
    ninja_content += "  description = LINK $out\n"
    ninja_content += "  rspfile = $out.rsp\n"
    ninja_content += "  rspfile_content = $in\n"
    ninja_content += "\n"

    ninja_content += "rule version\n"
    ninja_content += "  command = cd $root && ./version.sh --versionh=build/generated/version.h\n"
    ninja_content += "  description = VERSION $out\n"
    ninja_content += "\n"

    ninja_content += "rule ebml_header\n"
    ninja_content += "  command = $root/TOOLS/matroska.py --generate-header $out\n"
    ninja_content += "  description = EBML $out\n"
    ninja_content += "\n"

    ninja_content += "rule ebml_defs\n"
    ninja_content += "  command = $root/TOOLS/matroska.py --generate-definitions $out\n"
    ninja_content += "  description = EBML $out\n"
    ninja_content += "\n"

    ninja_content += "rule file2string\n"
    ninja_content += "  command = $root/TOOLS/file2string.py $in $out $root\n"
    ninja_content += "  description = INC $out\n"
    ninja_content += "\n"

    if wl_proto_dir:
        ninja_content += "rule wayland_code\n"
        ninja_content += "  command = $wayscan private-code $in $out\n"
        ninja_content += "  description = WAYSHC $out\n"
        ninja_content += "\n"

        ninja_content += "rule wayland_header\n"
        ninja_content += "  command = $wayscan client-header $in $out\n"
        ninja_content += "  description = WAYSHH $out\n"
        ninja_content += "\n"

    # Generate build statements for generated files
    ninja_content += "# Generated files\n"
    # Create an always-rebuild phony target to force version check on every build
    ninja_content += "build _version_check: phony\n"
    ninja_content += "\n"
    # Mark version.h as a generator that depends on the always-rebuild target
    ninja_content += f"build $builddir/generated/version.h: version _version_check\n"
    ninja_content += f"  generator = 1\n"
    ninja_content += "\n"

    ninja_content += f"build $builddir/generated/ebml_types.h: ebml_header\n"
    ninja_content += f"build $builddir/generated/ebml_defs.c: ebml_defs\n"
    ninja_content += "\n"

    # Generate .inc files for config files
    inc_files = [
        ("etc/input.conf", "$builddir/generated/etc/input.conf.inc"),
        ("etc/input_vo_gpu.conf", "$builddir/generated/etc/input_vo_gpu.conf.inc"),
        ("etc/input_vo_dmabuf_wayland.conf", "$builddir/generated/etc/input_vo_dmabuf_wayland.conf.inc"),
        ("etc/input_vo_wlshm.conf", "$builddir/generated/etc/input_vo_wlshm.conf.inc"),
        ("etc/input_vo_vdpau.conf", "$builddir/generated/etc/input_vo_vdpau.conf.inc"),
        ("etc/input_vo_x11.conf", "$builddir/generated/etc/input_vo_x11.conf.inc"),
        ("etc/input_vo_drm.conf", "$builddir/generated/etc/input_vo_drm.conf.inc"),
        ("etc/builtin.conf", "$builddir/generated/etc/builtin.conf.inc"),
        ("etc/dmpv-icon-8bit-16x16.png", "$builddir/generated/etc/dmpv-icon-8bit-16x16.png.inc"),
        ("etc/dmpv-icon-8bit-32x32.png", "$builddir/generated/etc/dmpv-icon-8bit-32x32.png.inc"),
        ("etc/dmpv-icon-8bit-64x64.png", "$builddir/generated/etc/dmpv-icon-8bit-64x64.png.inc"),
        ("etc/dmpv-icon-8bit-128x128.png", "$builddir/generated/etc/dmpv-icon-8bit-128x128.png.inc"),
        ("sub/osd_font.otf", "$builddir/generated/sub/osd_font.otf.inc"),
        ("player/lua/defaults.lua", "$builddir/generated/player/lua/defaults.lua.inc"),
        ("player/lua/assdraw.lua", "$builddir/generated/player/lua/assdraw.lua.inc"),
        ("player/lua/options.lua", "$builddir/generated/player/lua/options.lua.inc"),
        ("player/lua/stats.lua", "$builddir/generated/player/lua/stats.lua.inc"),
        ("player/lua/360-sbs.lua", "$builddir/generated/player/lua/360-sbs.lua.inc"),
        ("player/lua/360-sg.lua", "$builddir/generated/player/lua/360-sg.lua.inc"),
        ("player/lua/positioning.lua", "$builddir/generated/player/lua/positioning.lua.inc"),
    ]
    for src, dst in inc_files:
        ninja_content += f"build {dst}: file2string $root/{src}\n"
    ninja_content += "\n"

    # Generate wayland protocol files if wayland is enabled
    if wl_proto_dir and "wayland" in str(sources):
        wayland_protocols = [
            ("unstable/idle-inhibit", "idle-inhibit-unstable-v1"),
            ("stable/presentation-time", "presentation-time"),
            ("stable/xdg-shell", "xdg-shell"),
            ("unstable/xdg-decoration", "xdg-decoration-unstable-v1"),
            ("stable/viewporter", "viewporter"),
            ("unstable/linux-dmabuf", "linux-dmabuf-unstable-v1"),
            ("staging/fractional-scale", "fractional-scale-v1"),
            ("staging/cursor-shape", "cursor-shape-v1"),
            ("stable/tablet", "tablet-v2"),
            ("staging/xdg-activation", "xdg-activation-v1"),
            ("staging/fifo", "fifo-v1"),
            ("staging/color-management", "color-management-v1"),
            ("staging/single-pixel-buffer", "single-pixel-buffer-v1"),
        ]
        for proto_dir, proto_name in wayland_protocols:
            ninja_content += f"build $builddir/generated/wayland/{proto_name}.c: wayland_code $wl_proto_dir/{proto_dir}/{proto_name}.xml\n"
            ninja_content += f"build $builddir/generated/wayland/{proto_name}.h: wayland_header $wl_proto_dir/{proto_dir}/{proto_name}.xml\n"
        ninja_content += "\n"

    # Process sources and generate build statements
    ninja_content += "# Object files\n"
    objects = []
    for src in sources:
        # Replace Make variables with Ninja variables
        src_path = src.replace("$(BUILD)", "$builddir").replace("$(ROOT)", "$root")

        # Determine output object file path
        if src.endswith(".c"):
            obj = src.replace(".c", ".o")
        elif src.endswith(".rc"):
            obj = src.replace(".rc", ".o")
        else:
            continue

        # All object files go to build directory
        # For $(BUILD)/foo.o -> $builddir/foo.o
        # For $(ROOT)/bar.o -> $builddir/bar.o
        obj_path = obj.replace("$(BUILD)/", "$builddir/").replace("$(ROOT)/", "$builddir/")
        objects.append(obj_path)

        # Determine dependencies for generated files
        implicit_deps = []
        if "common/version.c" in src or "osdep/dmpv.c" in src:
            implicit_deps.append("$builddir/generated/version.h")
        if "demux/demux_mkv.c" in src or "demux/ebml.c" in src:
            implicit_deps.append("$builddir/generated/ebml_types.h")
            implicit_deps.append("$builddir/generated/ebml_defs.c")
        if "input/input.c" in src:
            implicit_deps.append("$builddir/generated/etc/input.conf.inc")
            implicit_deps.append("$builddir/generated/etc/input_vo_gpu.conf.inc")
            implicit_deps.append("$builddir/generated/etc/input_vo_dmabuf_wayland.conf.inc")
            implicit_deps.append("$builddir/generated/etc/input_vo_wlshm.conf.inc")
            implicit_deps.append("$builddir/generated/etc/input_vo_vdpau.conf.inc")
            implicit_deps.append("$builddir/generated/etc/input_vo_x11.conf.inc")
            implicit_deps.append("$builddir/generated/etc/input_vo_drm.conf.inc")
        if "player/main.c" in src:
            implicit_deps.append("$builddir/generated/etc/builtin.conf.inc")
        if "sub/osd_libass.c" in src:
            implicit_deps.append("$builddir/generated/sub/osd_font.otf.inc")
        if "player/lua.c" in src:
            implicit_deps.extend([
                "$builddir/generated/player/lua/defaults.lua.inc",
                "$builddir/generated/player/lua/assdraw.lua.inc",
                "$builddir/generated/player/lua/options.lua.inc",
                "$builddir/generated/player/lua/stats.lua.inc",
                "$builddir/generated/player/lua/360-sbs.lua.inc",
                "$builddir/generated/player/lua/360-sg.lua.inc",
                "$builddir/generated/player/lua/positioning.lua.inc",
            ])
        if "video/out/x11_common.c" in src:
            implicit_deps.extend([
                "$builddir/generated/etc/dmpv-icon-8bit-16x16.png.inc",
                "$builddir/generated/etc/dmpv-icon-8bit-32x32.png.inc",
                "$builddir/generated/etc/dmpv-icon-8bit-64x64.png.inc",
                "$builddir/generated/etc/dmpv-icon-8bit-128x128.png.inc",
            ])
        if "wayland" in src and wl_proto_dir:
            # Add wayland protocol dependencies
            for proto_dir, proto_name in wayland_protocols:
                implicit_deps.append(f"$builddir/generated/wayland/{proto_name}.h")

        # Generate build statement
        if src.endswith(".c"):
            if implicit_deps:
                ninja_content += f"build {obj_path}: cc {src_path} | {' '.join(implicit_deps)}\n"
            else:
                ninja_content += f"build {obj_path}: cc {src_path}\n"

    ninja_content += "\n"

    # Link target
    target = f"$builddir/dmpv$exesuf"
    # Add version.h as order-only dependency to ensure link runs when version changes
    # Use | to specify order-only dependency so it's not included in $in (and thus not in the response file)
    ninja_content += f"build {target}: link {' '.join(objects)} | $builddir/generated/version.h\n"
    ninja_content += "\n"

    # Default target
    ninja_content += f"default {target}\n"
    ninja_content += "\n"

    # Install/uninstall/clean rules
    prefix = _G.install_paths.get("PREFIX", "/usr/local")

    ninja_content += "rule install_rule\n"
    ninja_content += f"  command = mkdir -p {prefix}/bin {prefix}/share/icons/hicolor/16x16/apps {prefix}/share/icons/hicolor/32x32/apps {prefix}/share/icons/hicolor/64x64/apps {prefix}/share/icons/hicolor/128x128/apps {prefix}/share/icons/hicolor/scalable/apps {prefix}/share/icons/hicolor/symbolic/apps {prefix}/share/applications {prefix}/etc && install -v -m 0755 $builddir/dmpv{exesuf} {prefix}/bin/dmpv{exesuf} && install -v -m 0644 $root/etc/dmpv-icon-8bit-16x16.png {prefix}/share/icons/hicolor/16x16/apps/dmpv.png && install -v -m 0644 $root/etc/dmpv-icon-8bit-32x32.png {prefix}/share/icons/hicolor/32x32/apps/dmpv.png && install -v -m 0644 $root/etc/dmpv-icon-8bit-64x64.png {prefix}/share/icons/hicolor/64x64/apps/dmpv.png && install -v -m 0644 $root/etc/dmpv-icon-8bit-128x128.png {prefix}/share/icons/hicolor/128x128/apps/dmpv.png && install -v -m 0644 $root/etc/dmpv.svg {prefix}/share/icons/hicolor/scalable/apps/dmpv.svg && install -v -m 0644 $root/etc/dmpv-symbolic.svg {prefix}/share/icons/hicolor/symbolic/apps/dmpv-symbolic.svg && install -v -m 0644 $root/etc/dmpv.desktop {prefix}/share/applications/dmpv.desktop && install -v -m 0644 $root/etc/dmpv.conf {prefix}/etc/dmpv.conf\n"
    ninja_content += "  description = INSTALL\n"
    ninja_content += "\n"

    ninja_content += "rule uninstall_rule\n"
    ninja_content += f"  command = rm -fv {prefix}/bin/dmpv{exesuf} {prefix}/share/icons/hicolor/16x16/apps/dmpv.png {prefix}/share/icons/hicolor/32x32/apps/dmpv.png {prefix}/share/icons/hicolor/64x64/apps/dmpv.png {prefix}/share/icons/hicolor/128x128/apps/dmpv.png {prefix}/share/icons/hicolor/scalable/apps/dmpv.svg {prefix}/share/icons/hicolor/symbolic/apps/dmpv-symbolic.svg {prefix}/share/applications/dmpv.desktop {prefix}/etc/dmpv.conf\n"
    ninja_content += "  description = UNINSTALL\n"
    ninja_content += "\n"

    ninja_content += "rule clean\n"
    ninja_content += f"  command = rm -rf $builddir\n"
    ninja_content += "  description = CLEAN\n"
    ninja_content += "\n"

    # Phony targets
    # Note: install target does not depend on the build target to avoid
    # rebuilding files as root when running 'sudo make install'
    ninja_content += "build install: install_rule\n"
    ninja_content += "build uninstall: uninstall_rule\n"
    ninja_content += "build clean: clean\n"
    ninja_content += "\n"

    return ninja_content

# To be called at the end of user checks.
def finish():
    if not is_running():
        return

    is_fatal = False
    for key, val in _G.feature_opts.items():
        print("Unknown feature set on command line: %s" % key)
        if val == "yes":
            is_fatal = True
    if is_fatal:
        die("Unknown feature was force-enabled.")

    _G.config_h += "\n"
    add_config_h_define("CONFIGURATION", " ".join(sys.argv))
    def _resolve_install_path(val, max_iter=10):
        if val is None:
            return None
        import re
        res = val
        for _ in range(max_iter):
            changed_flag = [False]
            def repl(match):
                name = match.group(1)
                if name in _G.install_paths:
                    changed_flag[0] = True
                    return _G.install_paths[name]
                return match.group(0)
            new = re.sub(r"\$\(([^)]+)\)", repl, res)
            if not changed_flag[0]:
                res = new
                break
            res = new
        return res

    confload = _G.install_paths.get("CONFLOADDIR")
    if confload is None:
        confload = "$(PREFIX)/etc/$(PROJNAME)"
    confload_resolved = _resolve_install_path(confload)
    add_config_h_define("DMPV_CONFDIR", confload_resolved or confload)
    enabled_features = [x[0] for x in filter(lambda x: x[1], _G.dep_enabled.items())]
    add_config_h_define("FULLCONFIG", " ".join(sorted(enabled_features)))

    # Emit per-feature #defines in config.h using canonical HAVE_... names.
    # This avoids creating short lowercase macros (like 'alsa') that collide
    # with identifiers in the code. Produce names like HAVE_ALSA, HAVE_OSS, etc.
    import re
    def token_to_define(tok):
        # If token already looks like a C identifier and already starts with HAVE_,
        # return it unchanged.
        if re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', tok) and tok.startswith("HAVE_"):
            return tok
        # Otherwise synthesize HAVE_<UPPERCASE_TOKEN>, replacing illegal chars.
        s = re.sub(r'[^A-Za-z0-9]', '_', tok)   # replace non-alnum with underscore
        s = s.upper()
        if not s.startswith("HAVE_"):
            s = "HAVE_" + s
        # Ensure it is a valid identifier (fallback)
        if not re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', s):
            # sanitize again more aggressively
            s = re.sub(r'^[^A-Za-z_]+', '', s)
            if not s:
                s = "HAVE_FEATURE"
        return s

    _G.config_h += "\n"
    for tok in sorted(enabled_features):
        name = token_to_define(tok)
        _G.config_h += "#define %s 1\n" % name
        # Also export synthesized HAVE_* names into config.mak so make can
        # evaluate them at parse time (useful for conditional generation).
        try:
            add_config_mak_var(name, "1")
        except Exception:
            # best-effort: do not fail configure if exporting fails
            pass
    with open(os.path.join(_G.build_dir, "config.h"), "w") as f:
        f.write(_G.config_h)

    add_config_mak_var("BUILD", _G.build_dir)
    add_config_mak_var("ROOT", _G.root_dir)
    _G.config_mak += "\n"

    add_config_mak_var("EXESUF", ".exe" if _G.exe_format == "pe" else "")

    for name, _ in install_paths_info:
        add_config_mak_var(name, _G.install_paths[name])
    _G.config_mak += "\n"

    _G.config_mak += "CFLAGS = %s %s %s\n" % (" ".join(_G.cflags),
                                              os.environ.get("CPPFLAGS", ""),
                                              os.environ.get("CFLAGS", ""))
    _G.config_mak += "\n"
    _G.config_mak += "LDFLAGS = %s %s\n" % (" ".join(_G.ldflags),
                                            os.environ.get("LDFLAGS", ""))
    _G.config_mak += "\n"

    sources = []
    for s in _G.sources:
        # Prefix all source files with "$(ROOT)/". This is important for out of
        # tree builds, where configure/make is run from "somewhere else", and
        # not the source directory.
        # Generated sources need to be prefixed with "$(BUILD)/" (for the same
        # reason). Since we do not know whether a source file is generated, the
        # convention is that the user takes care of prefixing it.
        if not s.startswith("$(BUILD)"):
            # If the path already uses some other Make variable, reject it.
            assert not s.startswith("$") # no other variables which make sense
            # If configure registered a generated artifact (paths under generated/),
            # treat that as a build-time generated source and prefix with $(BUILD).
            # This avoids producing paths like $(ROOT)/generated/... which later
            # get an extra $(BUILD)/ prefix and leads to build/build/...
            if s.startswith("generated/"):
                s = "$(BUILD)/%s" % s
            else:
                s = "$(ROOT)/%s" % s
        sources.append(s)

    # Deduplicate sources (in case multiple features add the same source file)
    unique_sources = sorted(list(set(sources)))

    _G.config_mak += "SOURCES = \\\n"
    for s in unique_sources:
        _G.config_mak += "   %s \\\n" % s

    _G.config_mak += "\n"

    with open(os.path.join(_G.build_dir, "config.mak"), "w") as f:
        f.write("# Generated by configure.\n\n" + _G.config_mak)

    # Generate build.ninja for Ninja backend (use deduplicated sources)
    cflags_str = " ".join(_G.cflags) + " " + os.environ.get("CPPFLAGS", "") + " " + os.environ.get("CFLAGS", "")
    ldflags_str = " ".join(_G.ldflags) + " " + os.environ.get("LDFLAGS", "")
    ninja_content = _generate_ninja_file(unique_sources, cflags_str.strip(), ldflags_str.strip())
    with open(os.path.join(_G.build_dir, "build.ninja"), "w") as f:
        f.write(ninja_content)

    if _G.out_of_tree:
        try:
            os.symlink(os.path.join(_G.root_dir, "Makefile.new"), "Makefile")
        except FileExistsError:
            print("Not overwriting existing Makefile.")

    _G.log_file.write("--- Finishing successfully.\n")
    print("Done. You can run 'ninja -C %s' now." % _G.build_dir)

# Return whether to actually run configure tests, and whether results of those
# tests are available.
def is_running():
    return not _G.help_mode

# Each argument is an array or tuple, with the first element giving the
# dependency identifier, or "_" to match always fulfilled. The elements after
# this are added as source files if the dependency matches. This stops after
# the first matching argument.
def pick_first_matching_dep(*deps):
    winner = None
    for e in  deps:
        if (e[0] == "_" or dep_enabled(e[0])) and (winner is None):
            # (the odd indirection though winner is so that all dependency
            #  identifiers are checked for existence)
            winner = e[1:]
    if winner is not None:
        add_sources(*winner)
