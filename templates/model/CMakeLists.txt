nx_project({{project}} SOURCES src/game.cpp)

include(NxLuau)
nx_luau_project(nx2d "${NX_PROJECT_ASSETS}")
