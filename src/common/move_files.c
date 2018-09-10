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


#include "common/move_files.h"
#include "common/variables.h"
#include "common/debug.h"
#include "common/image.h"
#include <stdlib.h>
#include <stdio.h>
#include <sqlite3.h>

int dt_move_selected_images(const char *pattern, const int overwrite)
{
  sqlite3_stmt *stmt;
  int result = 0, seq = 1;

  // Get selected images
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // Count the number of failures
    if (dt_move_image(sqlite3_column_int(stmt, 0), pattern, seq, overwrite) > 0) {
      result++;
    }
    seq++;
  }
  sqlite3_finalize(stmt);

  return result;
}

int dt_move_image(const int id, const char *pattern, const int seq, const int overwrite)
{
  gchar current_image_path[PATH_MAX] = { 0 };
  gchar *targ_path = NULL;
  dt_variables_params_t *params = NULL;
  int err = 0;

  // Get the full current path of the image
  gboolean from_cache = FALSE;
  dt_image_full_path(id, current_image_path, sizeof(current_image_path), &from_cache);

  // Now start the work (thread-safely)

  // Synchronize sequence number to get target path for this image
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  {
    dt_variables_params_init(&params);
    params->filename = current_image_path;
    params->jobcode = "mv";
    params->imgid = id;
    params->sequence = seq;
    targ_path = dt_image_get_targ_path(params, pattern, overwrite);
  } // end of critical block; unsynchronize
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  char *output_dir = g_path_get_dirname(targ_path);

  if(g_mkdir_with_parents(output_dir, 0755))
  {
    fprintf(stderr, "[move_files] could not create directory: `%s'!\n", output_dir);
    //dt_control_log(_("could not create directory `%s'!"), output_dir);
    err = 10;
  }
  if(err == 0 && g_access(output_dir, W_OK | X_OK) != 0)
  {
    fprintf(stderr, "[move_files] could not write to directory: `%s'!\n", output_dir);
    //dt_control_log(_("could not write to directory `%s'!"), output_dir);
    err = 30;
  }

  // Done with output dir
  g_free(output_dir);

  if(err > 0) {
    return err;
  }

  printf("Moving image '%s' to path '%s'\n", current_image_path, targ_path);
  /*
  // Move image file (PSEUDO-CODE... TODO: Implement for real.)
  if (!mv_file(current_image_path, targ_image_path))
  {
  fprintf(stderr, "[move_files] could not move file: `%s'!\n", current_image_path);
  dt_control_log(_("could not move file `%s' to `%s'!"), current_image_path, targ_path);
  return 40;
  }

  printf("[move_job] moved file `%s' and accompanying sidecar files to `%s'\n", current_image_path, targ_path);
  //dt_control_log(ngettext("%d/%d  to `%s'", "%d/%d exported to `%s'", num), num, total, filename);
  */

  free(targ_path); targ_path = NULL;
  free(params); params = NULL;

  return 0;
}

char* dt_image_get_targ_path(dt_variables_params_t *params, const char* orig_pattern, const int overwrite)
{
  // Replace special characters in pattern, like `~`
  gchar *pattern = dt_util_fix_path(orig_pattern);
  char* targ_path = dt_variables_expand(params, pattern, TRUE);

  // If targ_path results in a directory (as indicated by trailing slash), add original filename
  char last_char = *(targ_path + strlen(targ_path) - 1);
  if (last_char == G_DIR_SEPARATOR_S[0])
  {
    // Get basename
    char* fname = g_path_get_basename(params->filename);

    // Drop the extension, if necessary
    char* ext = strrchr(fname, '.');
    if (ext) {
      *ext = 0;
    }

    // Merge filename onto base path
    char* tmp_path = (char*)calloc(sizeof(char), strlen(targ_path)+strlen(fname)+1);
    snprintf(tmp_path, sizeof(char)*(strlen(targ_path)+strlen(fname)+1), "%s%s", targ_path, fname);

    // Free old memory and move pointer
    free(targ_path); targ_path = NULL;
    targ_path = tmp_path;

    // Clean up
    free(fname); fname = NULL;
  }

  char *ext = strrchr(params->filename, '.');
  if (!ext) {
    ext = "";
  } else {
    ext++;
  }
  char *targ_path_end = targ_path + strlen(targ_path);
  size_t filename_free_space = sizeof(targ_path) - (targ_path_end - targ_path);
  snprintf(targ_path_end, filename_free_space, ".%s", ext);

  // prevent overwrite of files that might already be there (if desired)
  if(!overwrite) {
    int tag = 1;
    while(g_file_test(targ_path, G_FILE_TEST_EXISTS))
    {
      snprintf(targ_path_end, filename_free_space, "_%.2d.%s", tag, ext);
      tag++;
    }
  }
  free(pattern); pattern = NULL;

  return targ_path;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;

