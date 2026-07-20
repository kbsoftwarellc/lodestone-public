cmake_minimum_required(VERSION 3.22)
project(CairnMapMods)

# RE-UE4SS is cloned by CI (or a developer) next to this file, pinned to the
# exact commit of the UE4SS build shipped to the game (ABI must match):
#   git clone --recursive https://github.com/Okaetsu/RE-UE4SS.git
#   git -C RE-UE4SS checkout c2ac246447a8bcd92541070cb474044e7a2bbbe6
add_subdirectory(RE-UE4SS)
add_subdirectory(mods)
