#!/usr/bin/env python3

# Copyright 2025 David M. King
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
from pathlib import Path

from .boards import BoardInfo, list_boards
from .modules import list_modules
from .generator import GenerationOptions, ProjectGenerator


def build_parser() -> tuple[argparse.ArgumentParser, list]:
    parser = argparse.ArgumentParser(
        description="Initialize a new ESP-IDF project using idf-new templates."
    )
    parser.add_argument(
        "project_name",
        nargs="?",
        help="Name of the new project directory (prompted if omitted)",
    )
    parser.add_argument(
        "--board",
        help="Board ID (relative path under boards/, e.g., waveshare/wvshr185_round_touch)",
    )
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="Interactively list boards and let you pick one",
    )
    parser.add_argument(
        "--dest",
        type=Path,
        help="Optional destination directory (defaults to ./<project_name>)",
    )
    parser.add_argument(
        "--list-boards",
        action="store_true",
        help="Print available boards and exit",
    )
    parser.add_argument(
        "--list-modules",
        action="store_true",
        help="Print available modules and exit",
    )
    parser.add_argument(
        "--feature",
        action="append",
        dest="features",
        metavar="NAME",
        default=[],
        help="Include a board-specific feature (e.g. --feature tf_card). Repeatable.",
    )

    available_modules = list_modules()
    for mod in available_modules:
        parser.add_argument(
            f"--{mod.flag}",
            action="store_true",
            help=f"Include module: {mod.name}",
        )

    return parser, available_modules


def _format_screen(info: BoardInfo) -> str | None:
    screen = info.screen
    if not screen:
        return None
    parts: list[str] = []
    if screen.size_inches:
        parts.append(f"{screen.size_inches:g}\"")
    if screen.resolution:
        parts.append(str(screen.resolution))
    if screen.technology:
        parts.append(str(screen.technology))
    if screen.shape:
        parts.append(f"{screen.shape} shape")
    return " ".join(parts) if parts else None


def _format_traits(info: BoardInfo) -> list[str]:
    traits: list[str] = []
    if info.has_touch:
        traits.append("touch")
    if info.screen and info.screen.shape:
        traits.append(str(info.screen.shape))
    if info.screen and info.screen.technology:
        traits.append(str(info.screen.technology).lower())
    traits.extend(info.traits)
    if info.panel:
        traits.append(str(info.panel).lower())
    ordered: list[str] = []
    seen: set[str] = set()
    for trait in traits:
        key = trait.lower()
        if not key or key in seen:
            continue
        seen.add(key)
        ordered.append(trait)
    return ordered


def _ensure_boards_available() -> list[BoardInfo]:
    boards = list_boards()
    if not boards:
        raise SystemExit("No boards found under boards/. Add one before using idf-new.")
    return boards


def _prompt_board_selection(default: str | None = None) -> str:
    boards = _ensure_boards_available()
    print("Select a board:")
    for idx, info in enumerate(boards, start=1):
        print(f"  {idx}) {info.display_name} [{info.board_id}]")
    if default:
        print(f"Press Enter for default [{default}] or choose a number (q to cancel):")
    else:
        print("Enter the number of the board to use (or 'q' to cancel):")
    while True:
        choice = input("> ").strip()
        lower_choice = choice.lower()
        if lower_choice in {"q", "quit"}:
            raise SystemExit("Board selection cancelled")
        if not choice and default:
            return default
        try:
            index = int(choice)
        except ValueError:
            print("Please enter a number from the list or 'q' to quit.")
            continue
        if 1 <= index <= len(boards):
            return boards[index - 1].board_id
        print("Selection out of range. Try again.")


def _prompt_project_name(default: str) -> str:
    while True:
        value = input(f"Project name [{default}]: ").strip()
        if not value:
            return default
        if "/" in value or value == ".":
            print("Please enter a simple directory name (no slashes).")
            continue
        return value


def _handle_listing(args: argparse.Namespace, modules: list) -> bool:
    if args.list_boards:
        boards = list_boards()
        if boards:
            print("Available boards:\n")
            for info in boards:
                print(f"  - {info.board_id}")
                print(f"    Name: {info.display_name}")
                screen_desc = _format_screen(info)
                if screen_desc:
                    print(f"    Screen: {screen_desc}")
                traits = _format_traits(info)
                if traits:
                    print(f"    Tags: {', '.join(traits)}")
                print()
        else:
            print("No boards found under boards/.")
        return True

    if args.list_modules:
        if modules:
            # Group by category
            by_category: dict[str, list] = {}
            for mod in modules:
                by_category.setdefault(mod.category, []).append(mod)
            print("Available modules:")
            for category, mods in sorted(by_category.items()):
                print(f"  {category}:")
                for mod in mods:
                    print(f"    --{mod.flag}\t{mod.name}")
        else:
            print("No modules are currently registered.")
        return True

    return False


def main() -> None:
    parser, modules = build_parser()
    args = parser.parse_args()

    if _handle_listing(args, modules):
        return

    boards_available = _ensure_boards_available()
    default_board = boards_available[0].board_id if boards_available else "generic"

    board_id = args.board
    if args.interactive or not board_id:
        board_id = _prompt_board_selection(default=board_id or default_board)

    project_name = args.project_name or _prompt_project_name("generic_esp32")

    enabled_modules = [m.flag for m in modules if getattr(args, m.flag)]
    enabled_features = list(dict.fromkeys(args.features))

    options = GenerationOptions(
        project_name=project_name,
        board_id=board_id,
        destination=args.dest,
        feature_flags=enabled_features,
        module_flags=enabled_modules,
    )

    generator = ProjectGenerator(options)

    try:
        project = generator.generate()
    except (FileNotFoundError, FileExistsError) as exc:
        raise SystemExit(str(exc)) from exc

    print(f"Project created at {project.root}")
    if enabled_features:
        print("Board features: " + ", ".join(enabled_features))
    if enabled_modules:
        print("Modules: " + ", ".join(enabled_modules))

    print("Next steps:")
    print(f"  cd {project.root}")
    print("  idf.py set-target <esp32/esp32s3/etc>")
    print("  idf.py build flash monitor")


if __name__ == "__main__":
    main()
