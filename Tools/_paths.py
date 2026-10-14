"""Plugin paths shared by the codegen and audit scripts.

Tools/ lives at the plugin root, so every path is derived from this file's location and the
scripts work wherever the plugin is installed (Plugins/UEAgent in any project, or a bare clone).
"""
import os
import pathlib

PLUGIN_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOURCE_DIR = os.path.join(PLUGIN_ROOT, "Source", "UEAgent")
PRIVATE_DIR = os.path.join(SOURCE_DIR, "Private")
COMMANDS_DIR = os.path.join(PRIVATE_DIR, "Commands")
# The one catalog: the plugin's MCP server reads it at startup (and on `mcp.http.restart`).
CATALOG_JSON = os.path.join(PLUGIN_ROOT, "Resources", "command_catalog.json")
# Diff reports written by the scripts: build artifacts, kept out of git (Intermediate/ is ignored).
REPORT_DIR = os.path.join(PLUGIN_ROOT, "Intermediate", "Codegen")

# Same paths as pathlib objects, for the scripts that use pathlib.
PLUGIN_ROOT_P = pathlib.Path(PLUGIN_ROOT)
SOURCE_DIR_P = pathlib.Path(SOURCE_DIR)
COMMANDS_DIR_P = pathlib.Path(COMMANDS_DIR)
CATALOG_JSON_P = pathlib.Path(CATALOG_JSON)


def report_path(name: str) -> str:
    """Path of a diff report under Intermediate/Codegen, creating the folder."""
    os.makedirs(REPORT_DIR, exist_ok=True)
    return os.path.join(REPORT_DIR, name)


def plugin_relative(path: str) -> str:
    """A path relative to the plugin root, with forward slashes (for generated headers)."""
    return os.path.relpath(path, PLUGIN_ROOT).replace(os.sep, "/")
