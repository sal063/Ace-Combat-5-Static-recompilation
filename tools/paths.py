import os

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
PARENT = os.path.dirname(ROOT)

CONFIG = os.path.join(ROOT, "config")
GENERATED = os.path.join(ROOT, "generated")

DISC_NAME = "Ace Combat 5 - The Unsung War (USA) (En,Ja)"

DISC_CANDIDATES = (os.path.join(PARENT, DISC_NAME),
                   os.path.join(PARENT, "game", DISC_NAME))


def _find_disc():
    env = os.environ.get("AC5_DISC")
    if env:
        return env
    for cand in DISC_CANDIDATES:
        if os.path.exists(cand):
            return cand
    return DISC_CANDIDATES[0]


DISC = _find_disc()
GAME = os.path.join(DISC, "SLUS_208.51")

PS2SDK = os.environ.get("PS2SDK_DIR") or os.path.join(PARENT, "PS2SDK")
SDK_LIBS = os.path.join(PS2SDK, "sce", "ee", "lib")


def config(name):
    return os.path.join(CONFIG, name)


def require(path, what, hint):
    if not os.path.exists(path):
        raise SystemExit("%s not found:\n    %s\n%s" % (what, path, hint))
    return path


DISC_HINT = ("Set AC5_DISC to the extracted disc directory, or put it beside\n"
             "the repo (or under game/) as '%s'." % DISC_NAME)
SDK_HINT = ("Set PS2SDK_DIR to a PS2SDK checkout.  Only the signature matcher\n"
            "needs it, and its output is already in config/sdk_symbols.json --\n"
            "you only need the SDK to regenerate that.")


def game():
    return require(GAME, "The game executable (SLUS_208.51)", DISC_HINT)


def sdk_libs():
    return require(SDK_LIBS, "The PS2SDK EE libraries", SDK_HINT)
