# Merge Folders

A small Linux Dear ImGui app for combining several directory trees into one.
It always runs rsync in dry-run mode first, discovers overlapping files and
file/folder shape clashes, asks which copy to keep, then performs the merge.

## Behavior

- Add source folders in the order they should be layered.
- Choose one destination folder. A new final folder name may be typed directly.
- Run the mandatory dry run. No merge button exists before it succeeds.
- Conflicts default to keeping every copy. Extra copies are renamed with a
  suffix such as `_collision_0001.ext`, skipping names that already exist.
- Resolve conflicts individually, or use the rename, destination, and latest
  source bulk choices.
- Merge. Directories and unique files combine automatically. No unrelated
  destination files are deleted.

Source paths use a trailing slash when passed to rsync, so the contents of each
folder land directly in the destination. Rsync is launched with `fork` and
`execvp`, never a shell. Spaces and shell characters in paths stay literal.

The destination and any source may not contain one another. This prevents a
merge from recursively ingesting its own output. Symlinked directories are not
followed while conflicts are inspected.

## Build

Requirements are a C++20 compiler, CMake, OpenGL development files, X11
development files, and `rsync` on `PATH`. CMake fetches GLFW and Dear ImGui.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/merge-folders
```
