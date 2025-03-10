/* Goxel 3D voxels editor
 *
 * copyright (c) 2018 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Just include the imgui cpp files, so that we don't have to handle them
 * in the Scons file.
 */

// Prevent warnings with gcc.
#ifndef __clang__
#pragma GCC diagnostic push
#if __GNUC__ >= 8
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#endif

#define IMGUI_DEFINE_MATH_OPERATORS
// #define IMGUI_DISABLE_OBSOLETE_FUNCTIONS

#include "../ext_src/imgui/imgui.cpp"
#include "../ext_src/imgui/imgui_draw.cpp"
#include "../ext_src/imgui/imgui_widgets.cpp"
#include "../ext_src/imgui/imgui_tables.cpp"

#include "../ext_src/imgui/ImGuizmo.cpp"

#ifdef __clang__
#pragma GCC diagnostic pop
#endif