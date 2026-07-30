/* Goxel 3D voxels editor
 *
 * copyright (c) 2019-2022 Guillaume Chereau <guillaume@noctua-software.com>
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

#include "goxel.h"

int gui_about_popup(void *data)
{
    gui_text("Goxel " GOXEL_VERSION_STR " (fork)");
    gui_text("Maintained by Mark 'Grandy' Bishop");
    gui_text("https://github.com/GrandyB/goxel");
    gui_text("");
    gui_text_wrapped(
        "This fork extends upstream Goxel for voxel map making for games - "
        "large layered maps, doodad placement, terrain generation, "
        "game-oriented cameras, and export paths useful for titles such as "
        "Ace of Spades and similar voxel engines.");
    gui_text("");
    gui_text("Original author: Guillaume Chereau");
    gui_text("<guillaume@noctua-software.com>");
    gui_text("Upstream: https://goxel.xyz");
    if (!DEFINED(GOXEL_MOBILE)) gui_text("GPL 3 License");

    if (gui_collapsing_header("Credits", true)) {
        gui_text("Libraries:");
        gui_text("● dear imgui (https://github.com/ocornut/imgui)");
        gui_text("● stb (https://github.com/nothings/stb)");
        gui_text("● yocto-gl (https://github.com/xelatihy/yocto-gl)");
        gui_text("● uthash (https://troydhanson.github.io/uthash/)");
        gui_text("● inih (https://github.com/benhoyt/inih)");
        gui_text("● voxelizer (https://github.com/karimnaaji/voxelizer)");
        gui_text("● tinyobjloader (https://github.com/syoyo/tinyobjloader-c)");
        gui_text("● boostrap icons (https://icons.getbootstrap.com)");
        gui_text("● meshoptimizer (https://github.com/zeux/meshoptimizer)");
        gui_text("● imguizmo (https://github.com/CedricGuillemet/ImGuizmo)");
        gui_text("● tinyfiledialogs (https://sourceforge.net/projects/tinyfiledialogs/)");

        gui_text("Contributors:");
        gui_text("● Michal (https://github.com/YarlBoro)");
        gui_text("● Dustin Willis Webber <dustin.webber@gmail.com>");
        gui_text("● Pablo Hugo Reda <pabloreda@gmail.com>");
        gui_text("● Othelarian (https://github.com/othelarian)");
    }
    return gui_button("OK", 0, 0);
}

int gui_about_scripts_popup(void *data)
{
    char dir[1024] = "";
    const char *examples_url =
        "https://github.com/guillaumechereau/goxel/tree/master/data/scripts";

    if (sys_get_user_dir()) {
        snprintf(dir, sizeof(dir), "%s/scripts", sys_get_user_dir());
    }

    gui_text("Starting from version 0.12.0 Goxel adds experimental support "
             "for javascript plugins.");
    gui_text("Add your own scripts in the directory:\n%s.", dir);
    gui_text("See some examples at %s.", examples_url);
    return gui_button("OK", 0, 0);
}
