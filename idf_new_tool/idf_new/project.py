# Copyright 2025-2026 David M. King
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import shutil

from .manifest import merge_manifests


@dataclass(slots=True)
class Project:
	"""Represents a generated ESP-IDF project on disk."""

	name: str
	root: Path

	@property
	def main_dir(self) -> Path:
		return self.root / "main"

	def replace_text(self, target: Path | str, old: str, new: str, count: int = -1) -> None:
		path = Path(target)
		if not path.is_absolute():
			path = self.root / path
		text = path.read_text(encoding="utf-8")
		if old not in text:
			raise ValueError(f"'{old}' not found in {path}")
		path.write_text(text.replace(old, new, count), encoding="utf-8")

	def copy_into_main(self, source: Path, dest_name: str | None = None) -> Path:
		destination = self.main_dir / (dest_name or source.name)
		shutil.copy2(source, destination)
		return destination



def _sanitize_board_id(board_id: str) -> str:
	safe = board_id.replace("\\", "/").replace("/", "_").strip()
	return safe or "board"


def create_project(name: str, template_dir: Path, destination: Path | None = None) -> Project:
	if not template_dir.exists():
		raise FileNotFoundError(f"Template directory not found: {template_dir}")

	dest_dir = (destination or Path.cwd() / name).resolve()
	if dest_dir.exists():
		raise FileExistsError(f"Destination already exists: {dest_dir}")

	shutil.copytree(template_dir, dest_dir)
	project = Project(name=name, root=dest_dir)
	cmake_name = Path(name).name
	project.replace_text("CMakeLists.txt", "project(base_project)", f"project({cmake_name})", count=1)
	return project


def install_board(project: Project, board_dir: Path, board_id: str) -> Path:
	board_impl = board_dir / "board_impl.c"
	if not board_impl.exists():
		raise FileNotFoundError(f"Board '{board_id}' is missing board_impl.c")

	file_stub = _sanitize_board_id(board_id)
	destination = project.copy_into_main(board_impl, f"board_{file_stub}.c")
	project.replace_text(
		project.main_dir / "CMakeLists.txt",
		"board_impl.c",
		destination.name,
		count=1,
	)

	manifest_src = board_dir / "idf_component.yml"
	manifest_dst = project.main_dir / "idf_component.yml"
	merge_manifests(manifest_dst, manifest_src)

	cmake_extra_src = board_dir / "main.cmake.extra"
	cmake_extra_dst = project.main_dir / "main.cmake.extra"
	if cmake_extra_src.exists():
		content = cmake_extra_src.read_text(encoding="utf-8")
		cmake_extra_dst.write_text(content, encoding="utf-8")

	# Copy any extra C/C++/header sources so boards can bundle helpers
	for pattern in ("*.c", "*.cpp", "*.h"):
		for extra in board_dir.glob(pattern):
			if extra == board_impl:
				continue
			project.copy_into_main(extra)

	# Copy sdkconfig.defaults* files to project root
	for sdkconfig in board_dir.glob("sdkconfig.defaults*"):
		shutil.copy2(sdkconfig, project.root / sdkconfig.name)

	# Copy vendored components directory if present
	components_src = board_dir / "components"
	if components_src.is_dir():
		components_dst = project.root / "components"
		for component in components_src.iterdir():
			if component.is_dir():
				shutil.copytree(component, components_dst / component.name)

	return destination


def install_board_feature(project: Project, feature_dir: Path, name: str) -> None:
	"""Copy a board-local feature's files into the project."""
	manifest_dst = project.main_dir / "idf_component.yml"
	merge_manifests(manifest_dst, feature_dir / "idf_component.yml")

	cmake_extra_dst = project.main_dir / "main.cmake.extra"
	cmake_extra_src = feature_dir / "main.cmake.extra"
	if cmake_extra_src.exists():
		existing = cmake_extra_dst.read_text(encoding="utf-8") if cmake_extra_dst.exists() else ""
		cmake_extra_dst.write_text(
			existing.rstrip() + "\n" + cmake_extra_src.read_text(encoding="utf-8"),
			encoding="utf-8",
		)

	kconfig_src = feature_dir / "Kconfig"
	kconfig_dst = project.main_dir / "Kconfig.projbuild"
	if kconfig_src.exists():
		snippet = kconfig_src.read_text(encoding="utf-8").rstrip()
		if kconfig_dst.exists():
			existing = kconfig_dst.read_text(encoding="utf-8").rstrip()
			kconfig_dst.write_text(existing + "\n\n" + snippet + "\n", encoding="utf-8")
		else:
			kconfig_dst.write_text(snippet + "\n", encoding="utf-8")

	for pattern in ("*.c", "*.cpp", "*.h"):
		for src in feature_dir.glob(pattern):
			project.copy_into_main(src)
# Copyright 2025-2026 David M. King
# SPDX-License-Identifier: Apache-2.0
