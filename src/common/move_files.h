/*
    This file is part of darktable,
    copyright (c) 2010 tobias ellinghaus.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * Move (and possibly rename) one image using standard DT variable substitutions.
 *
 * If pattern ends in slash (or backslash), then it is considered a directory and all image
 * files and accompanying XMP files are moved into the resulting directory maintaining their
 * current names. Otherwise, the whole pattern is resolved to a final filename and the image
 * and accompanying XMP files are moved to the new name.
 *
 * @param int id The ID of the selected image (must be > 0)
 * @param char* pattern The pattern used to create the destination
 * @param const int seq The sequence number of this image (for thread safety)
 * @param const int overwrite Whether or not to overwrite an existing file at the destination
 *
 * Returns 0 on success, or the following numbers on failure:
 *
 * * 10 - Couldn't create destination file/directory
 * * 20 - Couldn't locate source file
 * * 30 - Couldn't write to destination directory
 * * 40 - Something else went wrong
 */
int dt_move_image(const int id, const char *pattern, const int seq, const int overwrite);

/**
 * Gets all selected images and calls dt_move_images on each.
 *
 * Returns 0 on success or the number of failures if any errors were encountered.
 */
int dt_move_selected_images(const char *pattern, const int overwrite);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;

