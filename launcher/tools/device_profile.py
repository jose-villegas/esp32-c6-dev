"""Read one device profile from device_profiles/, for Python callers.

The sh half is device_profile.sh, and the two parse the SAME files - see
that script's header for the format's rules and for why a profile may not
contain anything a shell would have to evaluate. This module is the reason
that restriction exists: it parses the files itself rather than shelling
out, so a Python gate does not need a POSIX sh on the machine.

    from device_profile import load, require
    p = load()                       # $DEVICE_PROFILE, default esp32c6
    ceiling = require(p, "DP_TEST_FRAME_CEILING_BYTES", int)

A field whose value is the literal "unmeasured" means no board of that kind
has been captured here; require() refuses it, so a gate cannot silently run
against a number nobody measured.
"""

import os
import shlex

PROFILE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "device_profiles")


class ProfileError(Exception):
    pass


def available(profile_dir=None):
    d = profile_dir or PROFILE_DIR
    if not os.path.isdir(d):
        return []
    return sorted(n[:-3] for n in os.listdir(d) if n.endswith(".sh"))


def load(name=None, profile_dir=None):
    """Parse a profile into a dict of str -> str."""
    name = name or os.environ.get("DEVICE_PROFILE") or "esp32c6"
    d = profile_dir or PROFILE_DIR
    path = os.path.join(d, name + ".sh")
    if not os.path.isfile(path):
        raise ProfileError("no profile %r in %s (available: %s)"
                           % (name, d, ", ".join(available(d)) or "none"))

    fields = {"DP_PROFILE_NAME": name, "DP_PROFILE_PATH": path}
    with open(path, "r", encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                raise ProfileError("%s:%d: not a KEY=value line: %s"
                                   % (path, lineno, line))
            key, value = line.split("=", 1)
            key = key.strip()
            # shlex, not a naive strip('"'), so an embedded space or an
            # apostrophe inside a provenance string survives intact.
            parts = shlex.split(value, comments=False, posix=True)
            fields[key] = parts[0] if parts else ""
    return fields


def require(profile, field, cast=str):
    """Read one field, failing loudly on missing or unmeasured values."""
    value = profile.get(field)
    name = profile.get("DP_PROFILE_NAME", "?")
    if value is None or value == "":
        raise ProfileError("profile %s has no %s" % (name, field))
    if value == "unmeasured":
        raise ProfileError(
            "profile %s's %s is unmeasured - no board of this kind has been\n"
            "captured here. Capture one and record the number in\n"
            "device_profiles/%s.sh, or run with DEVICE_PROFILE=esp32c6."
            % (name, field, name))
    try:
        return cast(value)
    except (TypeError, ValueError) as exc:
        raise ProfileError("profile %s's %s is not a %s: %r"
                           % (name, field, cast.__name__, value)) from exc


if __name__ == "__main__":
    import sys
    prof = load(sys.argv[1] if len(sys.argv) > 1 else None)
    for k in sorted(prof):
        print("%s=%s" % (k, prof[k]))
