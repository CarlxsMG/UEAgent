"""
integrate_property_handlers.py -- writes the generated property-handler regions into the handler .cpp files.

Each domain below names a handler .cpp, the descriptors whose commands it holds and an optional comment
for its region. The text between that file's `// <generated:property-handlers>` and
`// </generated:property-handlers>` lines is replaced with the code codegen_property_handlers.py emits
for those descriptors; the rest of the file is left as it is. Run it before the bridge and catalog
codegens, which read the C++. A second run with unchanged descriptors changes nothing.

Usage:
  python Tools/integrate_property_handlers.py            # every domain
  python Tools/integrate_property_handlers.py audio      # one domain
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _paths  # noqa: E402
import codegen_property_handlers as gen  # noqa: E402

OPEN_MARK = "// <generated:property-handlers>"
CLOSE_MARK = "// </generated:property-handlers>"

DOMAINS = {
    "environment": {
        "cpp": "Environment/UEAgentEnvironmentCommands.cpp",
        "descriptors": ["sky_atmosphere", "exponential_height_fog", "volumetric_cloud", "sky_light"],
        "comment": [
            "Each set handler applies the supplied params through the component's ENGINE_API setters, which update",
            "its render state. All but the fog's change nothing on a Static component (AreDynamicDataChangesAllowed());",
            "the components default to Movable (SkyLight: Stationary), so the setters apply headless too. The readers",
            "read back the public fields the setters write.",
        ],
    },
    "audio": {
        "cpp": "Audio/UEAgentAudioAssetCommands.cpp",
        "descriptors": ["sound_concurrency", "sound_attenuation"],
    },
}


def load_descriptor(name):
    with open(os.path.join(HERE, "property_descriptors", name + ".json"), encoding="utf-8") as fh:
        return json.load(fh)


def build_region(cfg):
    names = ",".join(cfg["descriptors"])
    lines = [
        f"{OPEN_MARK} -- DO NOT EDIT BY HAND. Source of truth:",
        f"// Tools/property_descriptors/{{{names}}}.json",
        "// Regenerate: python Tools/integrate_property_handlers.py",
        "",
        *(f"// {line}" for line in cfg.get("comment", [])),
    ]
    code = gen.emit_region([load_descriptor(name) for name in cfg["descriptors"]])
    return "\n".join(lines) + "\n" + code + "\n" + CLOSE_MARK


def integrate(cfg):
    path = os.path.join(_paths.COMMANDS_DIR, cfg["cpp"])
    with open(path, encoding="utf-8") as fh:
        src = fh.read()
    start = src.find(OPEN_MARK)
    end = src.find(CLOSE_MARK)
    if start < 0 or end < start:
        raise SystemExit(f"{cfg['cpp']}: add the lines `{OPEN_MARK}` and `{CLOSE_MARK}` where the region goes")
    new = src[:start] + build_region(cfg) + src[end + len(CLOSE_MARK):]
    if new == src:
        return "unchanged"
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(new)
    return "updated"


def main():
    only = set(sys.argv[1:])
    unknown = only - DOMAINS.keys()
    if unknown:
        raise SystemExit(f"unknown domain(s): {sorted(unknown)}; known: {sorted(DOMAINS)}")
    for domain, cfg in DOMAINS.items():
        if not only or domain in only:
            print(f"  {domain:14} -> {integrate(cfg)}")


if __name__ == "__main__":
    main()
