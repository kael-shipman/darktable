/*
    This file is part of darktable,
    copyright (c) 2014, 2015 marcello perathoner

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

/*
  This module implements an effect similar to the warp tool in Gimp
  2.9 (or the PS liquify tool).

  TODO:
    lock threads
    faster stamp computation
    more opencl / sse optimizations
    draw outlines instead of fills
    split / join paths
    multiple-segment manipulations
    help texts
    mesh tool
    cleanup cruft
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include <complex.h>
#undef NDEBUG
#include <assert.h>
#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <cairo.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef NDEBUG
#define PRINT(...)
#define PRINT_FUNC()
#define PRINT_FUNC_ARGS(...)
#else
#define PRINT(...)                              \
  dt_print (DT_DEBUG_DEV, __VA_ARGS__)
#define PRINT_FUNC()                                            \
  dt_print (DT_DEBUG_DEV, "iop::liquify::%s ()\n", __func__);
#define PRINT_FUNC_ARGS(...)                                    \
  dt_print (DT_DEBUG_DEV, "iop::liquify::%s (", __func__);      \
  dt_print (DT_DEBUG_DEV, __VA_ARGS__);                         \
  dt_print (DT_DEBUG_DEV, ")\n");
#endif

#define PERF_START()                            \
  dt_times_t perf_start;                        \
  dt_get_times (&perf_start);
#define PERF_STOP(...)                          \
  perf_stop (perf_start, __VA_ARGS__);

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(1, dt_iop_liquify_params_t)

const int    LOOKUP_OVERSAMPLE = 10;
const int    INTERPOLATION_POINTS = 100; // when interpolating bezier
const double STAMP_RELOCATION = 0.1;     // how many radii to move stamp forward when following a path

#define CONF_RADIUS "plugins/darkroom/liquify/radius"

/**
 * Enum of layers.
 *
 * Sorted back to front.
 */

typedef enum {
  DT_LIQUIFY_LAYER_BACKGROUND,
  DT_LIQUIFY_LAYER_RADIUS,
  DT_LIQUIFY_LAYER_HARDNESS1,
  DT_LIQUIFY_LAYER_HARDNESS2,
  DT_LIQUIFY_LAYER_WARPS,
  DT_LIQUIFY_LAYER_PATH,
  DT_LIQUIFY_LAYER_CTRLPOINT1_HANDLE,
  DT_LIQUIFY_LAYER_CTRLPOINT2_HANDLE,
  DT_LIQUIFY_LAYER_RADIUSPOINT_HANDLE,
  DT_LIQUIFY_LAYER_HARDNESSPOINT1_HANDLE,
  DT_LIQUIFY_LAYER_HARDNESSPOINT2_HANDLE,
  DT_LIQUIFY_LAYER_STRENGTHPOINT_HANDLE,
  DT_LIQUIFY_LAYER_CENTERPOINT,
  DT_LIQUIFY_LAYER_CTRLPOINT1,
  DT_LIQUIFY_LAYER_CTRLPOINT2,
  DT_LIQUIFY_LAYER_RADIUSPOINT,
  DT_LIQUIFY_LAYER_HARDNESSPOINT1,
  DT_LIQUIFY_LAYER_HARDNESSPOINT2,
  DT_LIQUIFY_LAYER_STRENGTHPOINT,
  DT_LIQUIFY_LAYER_LAST
} dt_liquify_layer_enum_t;

typedef enum {
  DT_LIQUIFY_LAYER_FLAG_HIT_TEST      =  1,   ///< include layer in hit testing
  DT_LIQUIFY_LAYER_FLAG_PREV_SELECTED =  2,   ///< show if previous node is selected
  DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED =  4,   ///< show if node is selected
  DT_LIQUIFY_LAYER_FLAG_POINT_TOOL    =  8,   ///< show if point tool active
  DT_LIQUIFY_LAYER_FLAG_LINE_TOOL     = 16,   ///< show if line tool active
  DT_LIQUIFY_LAYER_FLAG_CURVE_TOOL    = 32,   ///< show if line tool active
  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL     = 64,   ///< show if node tool active
  DT_LIQUIFY_LAYER_FLAG_ANY_TOOL      = 8 + 16 + 32 + 64,
} dt_liquify_layer_flag_enum_t;

typedef struct {
  double red, green, blue, alpha;
} dt_liquify_rgba_t;

#define COLOR_NULL                 { 0.0, 0.0, 0.0, 0.8 }
#define GREY                       { 0.3, 0.3, 0.3, 0.8 }
#define LGREY                      { 0.8, 0.8, 0.8, 1.0 }
#define COLOR_DEBUG                { 0.9, 0.9, 0.0, 1.0 }
static const dt_liquify_rgba_t DT_LIQUIFY_COLOR_SELECTED = { 1.0, 1.0, 1.0, 1.0 };
static const dt_liquify_rgba_t DT_LIQUIFY_COLOR_HOVER    = { 1.0, 1.0, 1.0, 0.8 };

typedef struct {
  dt_liquify_layer_enum_t hover_master;    ///< hover whenever master layer hovers, eg. to
  dt_liquify_rgba_t fg;                    ///< the foreground color for this layer
  dt_liquify_rgba_t bg;                    ///< the background color for this layer
  double opacity;                          ///< the opacity of this layer
                                           ///  highlight the whole radius when only the
                                           ///  radius point is hovered
  dt_liquify_layer_flag_enum_t flags;      ///< various flags for layer
  const char *hint;                        ///< hint displayed when hovering
} dt_liquify_layer_t;

dt_liquify_layer_t dt_liquify_layers[] = {
  { DT_LIQUIFY_LAYER_BACKGROUND,     COLOR_NULL,  COLOR_NULL, 0.0,  0,                                                                                                      },
  { DT_LIQUIFY_LAYER_RADIUS,         COLOR_DEBUG, COLOR_NULL, 0.25, DT_LIQUIFY_LAYER_FLAG_ANY_TOOL,                                                                         },
  { DT_LIQUIFY_LAYER_HARDNESS1,      COLOR_DEBUG, COLOR_NULL, 1.0,  0,                                                                                                      },
  { DT_LIQUIFY_LAYER_HARDNESS2,      COLOR_DEBUG, COLOR_NULL, 1.0,  0,                                                                                                      },
  { DT_LIQUIFY_LAYER_WARPS,          COLOR_DEBUG, LGREY,      0.5,  DT_LIQUIFY_LAYER_FLAG_ANY_TOOL,                                                                         },
  { DT_LIQUIFY_LAYER_PATH,           GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_ANY_TOOL  | DT_LIQUIFY_LAYER_FLAG_HIT_TEST,                                       },
  { DT_LIQUIFY_LAYER_CTRLPOINT1,     GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL,                                                                        },
  { DT_LIQUIFY_LAYER_CTRLPOINT2,     GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL,                                                                        },
  { DT_LIQUIFY_LAYER_RADIUSPOINT,    GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL,                                                                        },
  { DT_LIQUIFY_LAYER_HARDNESSPOINT1, GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED,                                  },
  { DT_LIQUIFY_LAYER_HARDNESSPOINT2, GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED,                                  },
  { DT_LIQUIFY_LAYER_STRENGTHPOINT,  GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_ANY_TOOL,                                                                         },
  { DT_LIQUIFY_LAYER_CENTERPOINT,    GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_ANY_TOOL  | DT_LIQUIFY_LAYER_FLAG_HIT_TEST,                                       },
  { DT_LIQUIFY_LAYER_CTRLPOINT1,     GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_HIT_TEST,                                       },
  { DT_LIQUIFY_LAYER_CTRLPOINT2,     GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_HIT_TEST,                                       },
  { DT_LIQUIFY_LAYER_RADIUSPOINT,    GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_HIT_TEST,                                       },
  { DT_LIQUIFY_LAYER_HARDNESSPOINT1, GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED | DT_LIQUIFY_LAYER_FLAG_HIT_TEST, },
  { DT_LIQUIFY_LAYER_HARDNESSPOINT2, GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED | DT_LIQUIFY_LAYER_FLAG_HIT_TEST, },
  { DT_LIQUIFY_LAYER_STRENGTHPOINT,  GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_ANY_TOOL  | DT_LIQUIFY_LAYER_FLAG_HIT_TEST,                                       }
};

typedef enum {
  DT_LIQUIFY_UI_WIDTH_THINLINE,
  DT_LIQUIFY_UI_WIDTH_THICKLINE,
  DT_LIQUIFY_UI_WIDTH_DOUBLELINE,
  DT_LIQUIFY_UI_WIDTH_GIZMO,
  DT_LIQUIFY_UI_WIDTH_GIZMO_SMALL,
  DT_LIQUIFY_UI_WIDTH_DEFAULT_RADIUS,
  DT_LIQUIFY_UI_WIDTH_DEFAULT_STRENGTH,
  DT_LIQUIFY_UI_WIDTH_MIN_DRAG,
  DT_LIQUIFY_UI_WIDTH_LAST
} dt_liquify_ui_width_enum_t;

double dt_liquify_ui_widths [] = {
  // value in 1/96 inch (that is: in pixels on a standard 96 dpi screen)
    1.0, // DT_LIQUIFY_UI_WIDTH_THINLINE
    3.0, // DT_LIQUIFY_UI_WIDTH_THICKLINE
    3.0, // DT_LIQUIFY_UI_WIDTH_DOUBLELINE
    8.0, // DT_LIQUIFY_UI_WIDTH_GIZMO
    6.0, // DT_LIQUIFY_UI_WIDTH_GIZMO_SMALL
  100.0, // DT_LIQUIFY_UI_WIDTH_DEFAULT_RADIUS,
   50.0, // DT_LIQUIFY_UI_WIDTH_DEFAULT_STRENGTH,
    4.0  // DT_LIQUIFY_UI_WIDTH_MIN_DRAG
};

typedef enum {
  DT_LIQUIFY_WARP_TYPE_LINEAR,         ///< A linear warp originating from one point.
  DT_LIQUIFY_WARP_TYPE_RADIAL_GROW,    ///< A radial warp originating from one point.
  DT_LIQUIFY_WARP_TYPE_RADIAL_SHRINK,
#if 0
  DT_LIQUIFY_WARP_TYPE_SWIRL_CCW,
  DT_LIQUIFY_WARP_TYPE_SWIRL_CW,
#endif
  DT_LIQUIFY_WARP_TYPE_LAST
} dt_liquify_warp_type_enum_t;

typedef enum {
  DT_LIQUIFY_NODE_TYPE_CUSP,
  DT_LIQUIFY_NODE_TYPE_SMOOTH,
  DT_LIQUIFY_NODE_TYPE_SYMMETRICAL,
  DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH,
  DT_LIQUIFY_NODE_TYPE_LAST
} dt_liquify_node_type_enum_t;

typedef enum {
  DT_LIQUIFY_STATUS_NONE = 0,
  DT_LIQUIFY_STATUS_NEW = 1,
  DT_LIQUIFY_STATUS_LAST
} dt_liquify_status_enum_t;

/**
 * Enumerates the shapes types we use.
 *
 * @DT_LIQUIFY_PATH_MOVE_TO_V1:     Same as #CAIRO_PATH_MOVE_TO.
 * @DT_LIQUIFY_PATH_LINE_TO_V1:     Same as #CAIRO_PATH_LINE_TO.
 * @DT_LIQUIFY_PATH_CURVE_TO_V1:    Same as #CAIRO_PATH_CURVE_TO.
 * @DT_LIQUIFY_PATH_CLOSE_PATH_V1:  Same as #CAIRO_PATH_CLOSE_PATH.
 *
 */

typedef enum {
  DT_LIQUIFY_PATH_MOVE_TO_V1 = 0,
  DT_LIQUIFY_PATH_LINE_TO_V1,
  DT_LIQUIFY_PATH_CURVE_TO_V1,
  DT_LIQUIFY_PATH_CLOSE_PATH_V1,
  DT_LIQUIFY_PATH_LAST
} dt_liquify_path_data_enum_t;

typedef struct {
  size_t size;
  dt_liquify_path_data_enum_t type;
  dt_liquify_node_type_enum_t node_type;
  dt_liquify_layer_enum_t selected;
  dt_liquify_layer_enum_t hovered;
} dt_liquify_path_header_t;

// Scalars and vectors are represented here as points because the only
// thing we can reasonably distort_transform are points.

typedef struct {
  double complex point;
  double complex strength;   ///< a point (the effective strength vector is: strength - point)
  double complex radius;     ///< a point (the effective radius scalar is: cabs (radius - point))
  double control1;           ///< range 0.0 .. 1.0 == radius
  double control2;           ///< range 0.0 .. 1.0 == radius
  dt_liquify_warp_type_enum_t type;
} dt_liquify_warp_t;

typedef struct {
  dt_liquify_path_header_t header;
  dt_liquify_warp_t warp;
} dt_liquify_move_to_v1_t;

typedef struct {
  dt_liquify_path_header_t header;
  dt_liquify_warp_t warp;
} dt_liquify_line_to_v1_t;

typedef struct {
  dt_liquify_path_header_t header;
  dt_liquify_warp_t warp;
  double complex ctrl1;
  double complex ctrl2;
} dt_liquify_curve_to_v1_t;

typedef struct {
  dt_liquify_path_header_t header;
} dt_liquify_close_path_v1_t;

/* typedef struct { */
/*   dt_liquify_path_header_t   header; */
/*   double complex topleft; */
/*   double complex bottomright; */
/*   size_t rows; */
/*   size_t cols; */
/*   GList *points; */
/* } dt_liquify_mesh_v1_t; */

// Set up lots of alternative ways to get at the popular members.

typedef union {
  struct {
    dt_liquify_path_header_t   header;
    union {
      double complex           point;
      dt_liquify_warp_t        warp;
    };
  };
  dt_liquify_move_to_v1_t    move_to_v1;
  dt_liquify_line_to_v1_t    line_to_v1;
  dt_liquify_curve_to_v1_t   curve_to_v1;
  dt_liquify_close_path_v1_t close_path_v1;
} dt_liquify_path_data_t;

typedef struct {
  dt_liquify_layer_enum_t layer;
  dt_liquify_path_data_t *elem;
} dt_liquify_hit_t;

static const dt_liquify_hit_t NOWHERE = { DT_LIQUIFY_LAYER_BACKGROUND, NULL };

typedef struct {
  size_t blob_size;
  int blob_version;
  char buffer [0];
} dt_iop_liquify_params_t;

typedef struct {
  int warp_kernel;
} dt_iop_liquify_global_data_t;

typedef struct {
  dt_pthread_mutex_t lock;
  GList *paths;         ///< All known paths. A list of lists. Owns all elements.

  double complex last_mouse_pos;
  double complex last_button1_pressed_pos;
  GdkModifierType last_mouse_mods;  ///< GDK modifiers at the time mouse button was pressed.

  dt_liquify_hit_t last_hit;      ///< Element last hit with mouse button.
  GList *dragging;      ///< List of dt_liquify_hit_t being dragged. Owns elements.

  dt_liquify_path_data_t *temp;    ///< Points to the element under construction or NULL.
  dt_liquify_status_enum_t status; ///< Various flags.

  cairo_t *fake_cr;     ///< A fake cairo context for hit testing and coordinate transform.

  gboolean mouse_pointer_in_widget;
  GtkLabel *label;
  GtkToggleButton *btn_no_tool, *btn_point_tool, *btn_line_tool, *btn_curve_tool, *btn_node_tool;

} dt_iop_liquify_gui_data_t;

// this returns a translatable name
const char *name ()
{
  return _("liquify");
}

int groups ()
{
  return IOP_GROUP_CORRECT;
}

int flags ()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

static void perf_stop (dt_times_t perf_start, const char *msg, ...)
{
  if (darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t perf_end;
    dt_get_times (&perf_end);

    char message[320];
    va_list ap;
    va_start (ap, msg);
    vsnprintf (message, sizeof (message), msg, ap);
    va_end (ap);

    char timing[80];
#ifdef _OPENMP
    snprintf (timing, sizeof (timing),
              "%0.04fs (%0.04fs CPU %d threads)",
              perf_end.clock - perf_start.clock,
              perf_end.user - perf_start.user,
              omp_get_max_threads ());
#else
    snprintf (timing, sizeof (timing),
              "%0.04fs (%0.04fs CPU)",
              perf_end.clock - perf_start.clock,
              perf_end.user - perf_start.user);
#endif

    dt_print (DT_DEBUG_PERF, "[liquify] %s in %s\n", message, timing);
  }
}

/******************************************************************************/
/* Code common to op-engine and gui.                                          */
/******************************************************************************/

/**
 * \defgroup Serialize Serialize / unserialize paths to blobs.
 * @{
 */

void debug_params (char *msg, dt_iop_liquify_params_t *params)
{
  PRINT_FUNC_ARGS ("%s", msg);
#if 0
  if (!params) {
    PRINT ("params is NULL!\n");
    return;
  }
  PRINT ("blob_size:    %ld\n", params->blob_size);
  PRINT ("blob_version: %d\n",  params->blob_version);

  unsigned char *p = (unsigned char *) &params->buffer;
  for (int i = 0; i < params->blob_size; )
  {
    PRINT ("%02x ", *p++);
    ++i;
    if (i % 16 == 0)
      PRINT ("\n");
    else if (i %  8 == 0)
      PRINT ("- ");
  }
  PRINT ("\n");
#endif
}

/**
 * Gets the size of a blob that holds all data in paths.
 *
 * @param   paths  A GList of GLists.
 *
 * @return  The size of the blob.
 */

static size_t get_blob_size (GList *paths)
{
  size_t size = 0;
  for (GList *i = paths; i != NULL; i = i->next)
  {
    size += sizeof (size_t); // a size_t for each path
    for (GList *j = i->data; j != NULL; j = j->next)
    {
      const dt_liquify_path_data_t *data = (dt_liquify_path_data_t *) j->data;
      size += data->header.size;
    };
  };
  return size;
}

/**
 * Serializes a paths list into a blob.
 *
 * Format of the blob is: foreach path a size_t size followed by the structs
 * making up the path.  The size is the size of the size field plus the size of
 * all structs following.
 *
 * @param paths   A Glist of GLists.
 * @param buffer  A buffer of sufficient size.  The size must be obtained
 *                by calling get_blob_size().
 */

static char *serialize_paths (GList *paths, char *buffer)
{
  char *p = buffer;
  for (GList *i = paths; i != NULL; i = i->next)
  {
    size_t *path_size = (size_t *) p;
    *path_size = sizeof (size_t);
    p += sizeof (size_t);
    for (GList *j = i->data; j != NULL; j = j->next)
    {
      const dt_liquify_path_data_t *data = (dt_liquify_path_data_t *) j->data;
      const size_t size = data->header.size;
      memcpy (p, data, size);
      p += size;
      *path_size += size;
    }
  }
  PRINT ("serialize_paths: buffer len = %ld\n", p - buffer);
  return p;
}

static GList *unserialize_paths (void *buffer, const size_t buflen)
{
  char *p = (char *) buffer;
  const char *buffer_end = p + buflen;
  GList *paths = NULL;

  while (p < buffer_end)
  {
    GList *path = NULL;
    const size_t path_len = * (size_t *) p;
    char *path_end = p + path_len;
    assert (path_end <= buffer_end);
    p += sizeof (size_t);
    while (p < path_end)
    {
      const dt_liquify_path_header_t *header = (dt_liquify_path_header_t *) p;
      size_t expected_size = 0;
      switch (header->type)
      {
      case DT_LIQUIFY_PATH_MOVE_TO_V1:
        expected_size = sizeof (dt_liquify_move_to_v1_t);
        break;
      case DT_LIQUIFY_PATH_LINE_TO_V1:
        expected_size = sizeof (dt_liquify_line_to_v1_t);
        break;
      case DT_LIQUIFY_PATH_CURVE_TO_V1:
        expected_size = sizeof (dt_liquify_curve_to_v1_t);
        break;
      case DT_LIQUIFY_PATH_CLOSE_PATH_V1:
        expected_size = sizeof (dt_liquify_close_path_v1_t);
        break;
      default:
        break;
      }
      if (expected_size == 0)
      {
        // forget this path, try the next one
        PRINT ("Bogus path data type %d\n", header->type);
        p = path_end;
        continue;
      }
      if (expected_size != header->size)
      {
        // forget this path, try the next one
        PRINT ("Bogus path data size (got %ld, expected %ld)\n", header->size, expected_size);
        p = path_end;
        continue;
      }
      void *data = malloc (header->size);
      memcpy (data, p, header->size);
      p += header->size;
      path = g_list_append (path, data);
    }
    assert (p <= path_end);
    paths = g_list_append (paths, path);
  }
  return paths;
}

static void serialize_params (const struct dt_iop_module_t *module, GList *paths)
{
  dt_iop_liquify_params_t *p = (dt_iop_liquify_params_t *) module->params;
  p->blob_size = get_blob_size (paths);
  p->blob_version = 1;
  serialize_paths (paths, p->buffer);
}

static GList *unserialize_params (dt_iop_liquify_params_t *params)
{
  if (params->blob_version == 1)
    return unserialize_paths (params->buffer, params->blob_size);
  return NULL;
}

static const double get_rot(const dt_liquify_warp_type_enum_t warp_type)
{
  double rot;
  switch (warp_type)
  {
  case DT_LIQUIFY_WARP_TYPE_RADIAL_SHRINK:
    rot = M_PI;
    break;
#if 0
  case DT_LIQUIFY_WARP_TYPE_SWIRL_CCW:
    rot = M_PI / 2.0;
    break;
  case DT_LIQUIFY_WARP_TYPE_SWIRL_CW:
    rot = M_PI * 3.0 / 2.0;
    break;
#endif
  default:
    rot = 0.0;
    break;
  }
  return rot;
}

/**@}*/


/**
 * \defgroup Distort transformations
 *
 * The functions in this group help transform between coordinate
 * systems.  (In darktable nomenclature this kind of transform is
 * called 'distort').
 *
 * The transforms between coordinate systems are not necessarily
 * perspective transforms (eg. lensfun), therefore no transformation
 * matrix can be specified for them, instead all points to be
 * transformed have to be passed through a darktable function.
 *
 * Note: only points may be sensibly 'distorted'. Vectors and scalars
 * don't have a meaningful 'distort'.
 *
 *
 * Explanation of the coordinate systems used by this module:
 *
 *
 * RAW: These are sensor coordinates. They go from x=0, y=0 to x=<sensor
 * width>, y=<sensor height>. In a landscape picture (rotated 0°) x=0,
 * y=0 will be top left. In a portrait picture (rotated 90°
 * counter-clockwise) x=0, y=0 will be bottom left.
 *
 * The user probably wants liquified regions to be anchored to the
 * motive when more transformations are added, eg. a different
 * cropping of the image.  For this to work, all coordinates we store
 * or pass between gui and pipe are RAW sensor coordinates.
 *
 *
 * PIECE: These are coordinates based on the size of our pipe piece.
 * They go from x=0, y=0 to x=<width of piece>, y=<height of piece>.
 * PIECE coordinates should only be used while processing an image.
 *
 * Note: Currently (as of darktable 1.7) there are no geometry
 * transforms between RAW and PIECE (our module coming very early in
 * the pipe), but this may change in a later release. By allowing for
 * them now, we are prepared for pipe order re-shuffeling.
 *
 *
 * CAIRO: These are coordinates based on the cairo view.  The extent
 * of the longest side of the cooked picture is normalized to 1.0.
 * x=0, y=0 is the top left of the cooked picture.  x=u, y=v is the
 * bottom right of a cooked picture with u<=1, v<=1 and either u==1 or
 * v==1 depending on orientation.  Note that depending on pan and zoom
 * cairo view borders and cooked picture borders may intersect in many
 * ways.
 *
 * The normalized scale helps in choosing default values for vectors and
 * radii.
 *
 *
 * VIEW: These are coordinates based on the cairo view. x=0, y=0 being
 * top left and x=<view width>, y=<view height> being bottom right.
 * The parameters to the mouse_moved, button_pressed, and
 * button_released functions are in this system.
 *
 * This system is also used for sizing ui-elements. They cannot be
 * expressed in CAIRO coordinates because they should not change size
 * when zooming the picture.
 *
 * To get sensible sizes for ui elements and default warps use this
 * relation between the scales: CAIRO * get_zoom_scale () == VIEW.
 *
 * @{
 */

typedef struct {
  dt_develop_t *develop;
  dt_dev_pixelpipe_t *pipe;
  float from_scale;
  float to_scale;
  bool direction; ///< to raw == false
  int pmin;
  int pmax;
} distort_params_t;

static double complex _distort_point (const double complex p, const distort_params_t *params)
{
  double complex q = p / params->from_scale;
  float pt[2] = { creal (q), cimag (q) };

  if (params->direction)
    dt_dev_distort_transform_plus     (params->develop, params->pipe, params->pmin, params->pmax, pt, 1);
  else
    dt_dev_distort_backtransform_plus (params->develop, params->pipe, params->pmin, params->pmax, pt, 1);

  q = (double) pt[0] + (double) pt[1] * I;
  q *= params->to_scale;

  PRINT_FUNC_ARGS ("%f %f -> %f %f", creal (p), cimag (p), creal (q), cimag (q));
  return q;
}

/**
 * Distort a list of points.
 *
 * @param list    A list of pointers to double complex points. Owned by caller.
 * @param params
 */

static void _distort_point_list (GList *list, const distort_params_t *params)
{
  const size_t len = g_list_length (list);
  float *buffer = malloc (2 * sizeof (float) * len);
  float *b = buffer;

  for (GList *i = list; i != NULL; i = i->next)
  {
    double complex p = *((double complex *) i->data) / params->from_scale;
    *b++ = (float) creal (p);
    *b++ = (float) cimag (p);
  }

  if (params->direction)
    dt_dev_distort_transform_plus     (params->develop, params->pipe, params->pmin, params->pmax, buffer, len);
  else
    dt_dev_distort_backtransform_plus (params->develop, params->pipe, params->pmin, params->pmax, buffer, len);

  b = buffer;
  for (GList *i = list; i != NULL; i = i->next)
  {
    double complex *p = (double complex *) i->data;
    PRINT_FUNC_ARGS ("%f %f -> %f %f", creal (*p), cimag (*p), b[0] * params->to_scale, b[1] * params->to_scale);
    *p = (b[0] + b[1] * I) * params->to_scale;
    b += 2;
  }
  free (buffer);
}

static void _distort_paths (const distort_params_t *params, GList *paths)
{
  GList *list = NULL;

  for (GList *i = paths; i != NULL; i = i->next)
  {
    for (GList *j = i->data; j != NULL; j = j->next)
    {
      dt_liquify_path_data_t *data = (dt_liquify_path_data_t *) j->data;
      switch (data->header.type)
      {
      case DT_LIQUIFY_PATH_CURVE_TO_V1:
        list = g_list_append (list, &data->curve_to_v1.ctrl1);
        list = g_list_append (list, &data->curve_to_v1.ctrl2);
        // fall thru
      case DT_LIQUIFY_PATH_MOVE_TO_V1:
      case DT_LIQUIFY_PATH_LINE_TO_V1:
        list = g_list_append (list, &data->warp.point);
        list = g_list_append (list, &data->warp.strength);
        list = g_list_append (list, &data->warp.radius);
        break;
      default:
        break;
      }
    }
  }
  _distort_point_list (list, params);
  g_list_free (list);
}

#define RAW_SCALE   (piece->pipe->iscale)
#define CAIRO_SCALE (1.0 / MAX (piece->pipe->backbuf_width, piece->pipe->backbuf_height))
#define UI_SCALE    (RAW_SCALE / zoom_scale)

static void distort_paths_raw_to_cairo (const struct dt_iop_module_t *module,
                                        const dt_dev_pixelpipe_iop_t *piece,
                                        GList *paths)
{
  PRINT_FUNC_ARGS ("raw_scale=%f, cairo_scale=%f", RAW_SCALE, CAIRO_SCALE);

  const distort_params_t params = { module->dev, piece->pipe, RAW_SCALE, CAIRO_SCALE, true, 0, 99999 };
  _distort_paths (&params, paths);
}

static void distort_paths_raw_to_piece (const struct dt_iop_module_t *module,
                                        const dt_dev_pixelpipe_iop_t *piece,
                                        const double roi_in_scale,
                                        GList *paths)
{
  PRINT_FUNC_ARGS ("raw_scale=%f, roi_in_scale=%f", RAW_SCALE, roi_in_scale);

  const distort_params_t params = { module->dev, piece->pipe, RAW_SCALE, roi_in_scale, true, 0, module->priority };
  _distort_paths (&params, paths);
}

static double complex distort_point_cairo_to_raw (const struct dt_iop_module_t *module,
                                                  const dt_dev_pixelpipe_iop_t *piece,
                                                  const double complex p)
{
  const distort_params_t params = { module->dev, piece->pipe, CAIRO_SCALE, RAW_SCALE, false, 0, 99999 };
  return _distort_point  (p, &params);
}

static double complex transform_view_to_cairo (const struct dt_iop_module_t *module,
                                               const dt_dev_pixelpipe_iop_t *piece,
                                               const double x,
                                               const double y)
{
  float pt[2];
  dt_dev_get_pointer_zoom_pos (module->dev, x, y, &pt[0], &pt[1]);

  pt[0] += 0.5;
  pt[1] += 0.5;
  // int w = piece->pipe->backbuf_width;
  // int h = piece->pipe->backbuf_height;
  const int w = piece->pipe->processed_width;
  const int h = piece->pipe->processed_height;
  float max = MAX (w, h);
  pt[0] *= w / max;
  pt[1] *= h / max;

  // PRINT_FUNC_ARGS ("%.2f %.2f -> %f %f", x, y, pt[0], pt[1]);
  return pt[0] + pt[1] * I;
}

static GList *copy_paths (GList *paths)
{
  GList *paths_copy = NULL;

  for (GList *i = paths; i != NULL; i = i->next)
  {
    GList *path_copy = NULL;
    for (GList *j = i->data; j != NULL; j = j->next)
    {
      const dt_liquify_path_data_t *data = (dt_liquify_path_data_t *) j->data;
      dt_liquify_path_data_t *data_copy = malloc (data->header.size);
      memcpy (data_copy, data, data->header.size);
      path_copy = g_list_append (path_copy, data_copy);
    }
    paths_copy = g_list_append (paths_copy, path_copy);
  }
  return paths_copy;
}

static void free_paths (GList *paths)
{
  for (GList *i = paths; i != NULL; i = i->next)
    g_list_free_full (i->data, free);
}

/**@}*/


/******************************************************************************/
/* Op-engine code.                                                            */
/******************************************************************************/

/**
 * Normalize a vector.
 *
 * @param v
 *
 * @return Normalized vector.
 */

static inline double complex normalize (const double complex v)
{
  if (cabs (v) < 0.000001)
    return 1.0;
  return v / cabs (v);
}

/**
 * Calculate the linear blend of scalars a and b.  This function is
 * sometimes known as `lerp´. We use the name `mix´ because that is
 * the name in opencl.
 *
 * @param   a
 * @param   b
 * @param   t
 *
 * @return  Interpolated point.
 */

static inline double mix (const double a, const double b, const double t)
{
  return a + (b - a) * t;
}

/**
 * Calculate the linear blend of points p0 and p1.  This function is
 * sometimes known as `lerp´. We use the name `mix´ because that is
 * the name in opencl.
 *
 * @param   p0
 * @param   p1
 * @param   t
 *
 * @return  Interpolated point.
 */

static inline double complex cmix (const double complex p0, const double complex p1, const double t)
{
  return p0 + (p1 - p0) * t;
}

/**
 * Calculate the linear blend of points p0 and p1.  This function is
 * sometimes known as `lerp´. We use the name `mix´ because that is
 * the name in opencl.
 *
 * @param   p0
 * @param   p1
 * @param   t
 *
 * @return  Interpolated point.
 */

static inline float complex cmixf (const float complex p0, const float complex p1, const float t)
{
  return p0 + (p1 - p0) * t;
}

struct __attribute__ ((aligned (16))) {
  __m128 three;
  __m128 threehalfs;
  __m128 half;
} _mm_constants;

/**
 * Fast parallel sqrt.  Using SSE rsqrt followed by one Reciproot iteration.
 *
 * See: http://en.wikipedia.org/wiki/Fast_inverse_square_root#Newton.27s_method
 *      http://www.netlib.org/fdlibm/e_sqrt.c
 *
 * @param x  4 single float input values
 *
 * @return  4 single float values of sqrt (x)
 */

static inline __m128 sqrt_ps (const __m128 x)
{
  const __m128 y    = _mm_rsqrt_ps (x);
  const __m128 x2   = _mm_mul_ps (_mm_constants.half, x);
  const __m128 yy   = _mm_mul_ps (y, y);
  const __m128 x2yy = _mm_mul_ps (x2, yy);
  return _mm_mul_ps (yy, _mm_sub_ps (_mm_constants.threehalfs, x2yy));
}

static void mix_warps (dt_liquify_warp_t *result,
                       const dt_liquify_warp_t *warp1,
                       const dt_liquify_warp_t *warp2,
                       const complex double pt,
                       const double t)
{
  result->type     = warp1->type;
  result->control1 = mix  (warp1->control1, warp2->control1, t);
  result->control2 = mix  (warp1->control2, warp2->control2, t);

  const double radius    = mix (cabs (warp1->radius - warp1->point), cabs (warp2->radius - warp2->point), t);
  result->radius   = pt + radius;

  const double r         = mix (cabs (warp1->strength - warp1->point), cabs (warp2->strength - warp2->point), t);
  const double phi       = mix (carg (warp1->strength - warp1->point), carg (warp2->strength - warp2->point), t);
  result->strength = pt + r * cexp (phi * I);

  result->point    = pt;
}

void debug_rect (const char *msg, const cairo_rectangle_int_t *r)
{
  PRINT ("%s  x=%4d y=%4d  x2=%4d y2=%4d  w=%4d h=%4d\n",
         msg, r->x, r->y, r->x + r->width, r->y + r->height, r->width, r->height);
}

void debug_roi (const char *label, const dt_iop_roi_t *roi)
{
  PRINT ("%s  x=%4d y=%4d  x2=%4d y2=%4d  w=%4d h=%4d  (scale=%f)\n",
         label, roi->x, roi->y, roi->x + roi->width, roi->y + roi->height,
         roi->width, roi->height, roi->scale);
}

void debug_piece (const dt_dev_pixelpipe_iop_t *piece)
{
  PRINT ("piece->buf_in: w=%4d h=%4d\n",
         piece->buf_in.width, piece->buf_in.height);
}


/**
 * Interpolate a cubic bezier spline into a series of points.
 *
 * @param  p0      The starting point of the bezier.
 * @param  p1      First control point.
 * @param  p2      Second control point.
 * @param  p3      End point.
 * @param  buffer  Preallocated buffer of size n.
 * @param  n       Number of points to interpolate. buffer[0] = p0 and buffer[n-1] = p3.
 */

static void interpolate_cubic_bezier (const float complex p0,
                                      const float complex p1,
                                      const float complex p2,
                                      const float complex p3,
                                      float complex buffer[],
                                      const int n)
{
  // convert from bernstein basis to polynomial basis to get faster math
  // See: http://www.tinaja.com/glib/cubemath.pdf
  const float complex A = p3 - 3 * p2 + 3 * p1 -     p0;
  const float complex B =      3 * p2 - 6 * p1 + 3 * p0;
  const float complex C =               3 * p1 - 3 * p0;
  const float complex D =                            p0;

  float complex *buf = buffer;
  const float step = 1.0 / n;
  float t = step;
  *buf++ = p0;

  for (int i = 1; i < n - 1; ++i)
  {
    *buf++ = ((((A) * t) + B) * t + C) * t + D;
    t += step;
  }
  *buf = p3;
}

static GList *interpolate_paths (GList *paths);

/**
 * Get approx. arc length of a curve.
 *
 * Used to approximate the arc length of a bezier curve.
 *
 * FIXME: for added hack value we could use cubic interpolation here.
 *
 * @param  points    Array of points.
 * @param  n_points  No. of points in array.
 */

static const double get_arc_length (const float complex points[], const int n_points)
{
  double length = 0.0;
  for (int i = 1; i < n_points; i++)
    length += cabs (points[i-1] - points[i]);
  return length;
}

typedef struct {
  int i;
  double length;
} restart_cookie_t;

/**
 * Interpolate a point on a curve at a specified arc length.
 *
 * In a bezier curve the parameter t usually does not correspond to
 * the arc length.
 *
 * Can use a restart cookie if called in a loop with increasing values
 * of arc_length.
 *
 * FIXME: for added hack value we could use cubic interpolation here.
 *
 * @param  points      Array of points.
 * @param  n_points    No. of points in array.
 * @param  arc_length  Arc length.
 * @param  restart     Restart cookie or NULL.
 */

static const float complex point_at_arc_length (const float complex points[],
                                                const int n_points,
                                                const double arc_length,
                                                restart_cookie_t *restart)
{
  double length = restart ? restart->length : 0.0;
  int i         = restart ? restart->i      : 1;

  for ( ; i < n_points; i++)
  {
    const double prev_length = length;
    length += cabsf (points[i-1] - points[i]);
    if (length >= arc_length) {
      const double t = (arc_length - prev_length) / (length - prev_length);
      if (restart)
      {
        restart->i = i;
        restart->length = prev_length;
      }
      return cmixf (points[i-1], points[i], t);
    }
  }

  return points[n_points - 1];
}

/**
 * Build a lookup table for the warp intensity.
 *
 * Lookup table for the warp intensity function: f(x). The warp
 * intensity function determines how much a pixel is influenced by the
 * warp depending from its distance from a central point.
 *
 * Boundary conditions: f(0) must be 1 and f(@a distance) must be 0.
 * f'(0) and f'(@a distance) must both be 0 or we'll get artefacts on
 * the picture.
 *
 * Implementation: a bezier curve with p0 = 0, 1 and p3 = 1, 0. p1 is
 * defined by @a control1, 1 and p2 by @a control1, 0.  Because a
 * bezier is parameterized on t, we have to reparameterize on x, which
 * we do by linear interpolation.
 *
 * Octave code:
 *
 * t = linspace (0,1,100);
 * grid;
 * hold on;
 * for steps = 0:0.1:1
 *   cpoints = [0,1; steps,1; steps,0; 1,0];
 *   bezier = cbezier2poly (cpoints);
 *   x = polyval (bezier(1,:), t);
 *   y = polyval (bezier(2,:), t);
 *   plot (t, interp1 (x, y, t));
 * end
 * hold off;
 *
 *
 * @param  distance  The value at which f(x) becomes 0.
 * @param  control1  x-value of bezier control point 1.
 * @param  control2  x-value of bezier control point 2.
 *
 * @return The lookup table of size @a distance + 1. The calling
 *         function must free the table.
 */

static float *
build_lookup_table (const int distance, const float control1, const float control2)
{
  PERF_START();

  float complex *clookup = dt_alloc_align (16, (distance + 1) * sizeof (float complex));

  interpolate_cubic_bezier (I, control1 + I, control2, 1.0, clookup, distance + 1);

  // reparameterize bezier by x and keep only y values

  float *lookup = dt_alloc_align (16, (distance + 1) * sizeof (float));
  float *ptr = lookup;
  float complex *cptr = clookup + 1;
  const float step = 1.0 / (float) distance;
  float x = 0.0;

  *ptr++ = 1.0;
  for (int i = 1; i < distance; i++)
  {
    x += step;
    while (creal (*cptr) < x)
      cptr++;
    const float dx1 = creal (cptr[0] - cptr[-1]);
    const float dx2 = x - creal (cptr[-1]);
    *ptr++ = cimag (cptr[0]) + (dx2 / dx1) * (cimag (cptr[0]) - cimag (cptr[-1]));
  }
  *ptr++ = 0.0;

  dt_free_align (clookup);
  PERF_STOP ("lookup table of length %d computed", distance + 1);
  return lookup;
}

static void compute_round_stamp_extent (cairo_rectangle_int_t *stamp_extent,
                                        const dt_liquify_warp_t *warp)
{

  const int iradius = round (cabs (warp->radius - warp->point));
  assert (iradius > 0);

  stamp_extent->x = stamp_extent->y = -iradius;
  stamp_extent->x += creal (warp->point);
  stamp_extent->y += cimag (warp->point);
  stamp_extent->width = stamp_extent->height = 2 * iradius + 1;
}

/**
 * Compute a round (circular) stamp.
 *
 * The stamp is a vector field of warp vectors around a center point.
 *
 * In a linear warp the center point gets a warp of @a strength, while
 * points on the circumference of the circle get no warp at all.
 * Between center and circumference the warp magnitude tapers off
 * following a curve (see: build_lookup_table()).
 *
 * Note that when applying a linear stamp to a path, we will first rotate its
 * vectors into the direction of the path.
 *
 * In a radial warp the center point and the points on the
 * circumference get no warp. Between center and circumference the
 * warp magnitude follows a curve with maximum at radius / 0.5
 *
 * Our stamp is stored in a rectangular region.
 *
 * @param pstamp[out]         A pointer to the stamp. Caller must free.
 * @param stamp_extent[out]   The extent of the field.
 * @param warp                A struct defining the warp params.
 */

static void build_round_stamp (float complex **pstamp,
                               cairo_rectangle_int_t *stamp_extent,
                               const dt_liquify_warp_t *warp)
{
  const int iradius = round (cabs (warp->radius - warp->point));
  assert (iradius > 0);

  stamp_extent->x = stamp_extent->y = -iradius;
  stamp_extent->width = stamp_extent->height = 2 * iradius + 1;

  // 0.5 is factored in so the warp starts to degenerate when the
  // strength arrow crosses the warp radius.
  const double complex strength = 0.5 * (warp->strength - warp->point);
  const double abs_strength = cabs (strength);

  PERF_START ();

  float complex *stamp = malloc (sizeof (float complex)
                                 * stamp_extent->width * stamp_extent->height);

  // clear memory
  #pragma omp parallel for schedule (static) default (shared)

  for (int i = 0; i < stamp_extent->height; i++)
  {
    float complex *row = stamp + i * stamp_extent->width;
    memset (row, 0, sizeof (float complex) * stamp_extent->width);
  }

  // lookup table: map of distance from center point => warp
  const int table_size = iradius * LOOKUP_OVERSAMPLE;
  const float *lookup_table = build_lookup_table (table_size, warp->control1, warp->control2);

  // points into buffer at the center of the circle
  float complex *center = stamp + 2 * iradius * iradius + 2 * iradius;

  // The expensive operation here is hypotf ().  By dividing the
  // circle in octants and doing only the inside we have to calculate
  // hypotf only for PI / 32 = 0.098 of the stamp area.
  #pragma omp parallel for schedule (dynamic, 1) default (shared)

  for (int y = 0; y <= iradius; y++)
  {
    for (int x = y; x <= iradius; x++)
    {
      const float dist = hypotf (x, y);
      const int idist = round (dist * LOOKUP_OVERSAMPLE);
      if (idist >= table_size)
        // idist will only grow bigger in this row
        goto next_row;

      // pointers into the 8 octants of the circle
      // octant count is ccw from positive x-axis
      float complex *o1 = center - y * stamp_extent->width + x;
      float complex *o2 = center - x * stamp_extent->width + y;
      float complex *o3 = center - x * stamp_extent->width - y;
      float complex *o4 = center - y * stamp_extent->width - x;
      float complex *o5 = center + y * stamp_extent->width - x;
      float complex *o6 = center + x * stamp_extent->width - y;
      float complex *o7 = center + x * stamp_extent->width + y;
      float complex *o8 = center + y * stamp_extent->width + x;

      float abs_lookup = abs_strength * lookup_table[idist] / iradius;

      switch (warp->type)
      {
      case DT_LIQUIFY_WARP_TYPE_RADIAL_GROW:
        *o1 = abs_lookup * ( x - y * I);
        *o2 = abs_lookup * ( y - x * I);
        *o3 = abs_lookup * (-y - x * I);
        *o4 = abs_lookup * (-x - y * I);
        *o5 = abs_lookup * (-x + y * I);
        *o6 = abs_lookup * (-y + x * I);
        *o7 = abs_lookup * ( y + x * I);
        *o8 = abs_lookup * ( x + y * I);
        break;

      case DT_LIQUIFY_WARP_TYPE_RADIAL_SHRINK:
        *o1 = -abs_lookup * ( x - y * I);
        *o2 = -abs_lookup * ( y - x * I);
        *o3 = -abs_lookup * (-y - x * I);
        *o4 = -abs_lookup * (-x - y * I);
        *o5 = -abs_lookup * (-x + y * I);
        *o6 = -abs_lookup * (-y + x * I);
        *o7 = -abs_lookup * ( y + x * I);
        *o8 = -abs_lookup * ( x + y * I);
        break;

#if 0
      case DT_LIQUIFY_WARP_TYPE_SWIRL_CW:
        *o1 =  abs_lookup * (3.0 * y - 5.0 * x * I);
        break;
      case DT_LIQUIFY_WARP_TYPE_SWIRL_CCW:
        *o1 = -abs_lookup * (3.0 * y - 5.0 * x * I);
        break;
#endif

      default:
        *o1 = *o2 = *o3 = *o4 = *o5 = *o6 = *o7 = *o8 =
          strength * lookup_table[idist];
        break;
      }
    }
  next_row: ; // ";" makes compiler happy
  }

  dt_free_align ((void *) lookup_table);
  *pstamp = stamp;

  PERF_STOP ("round stamp of size %dx%d computed", stamp_extent->width, stamp_extent->height);
}

/**
 * Applies a stamp at a specified position.
 *
 * Applies a stamp at the position specified by @a point and adds the
 * resulting vector field to the global distortion map @a global_map.
 *
 * The global distortion map is a map of relative pixel displacements
 * encompassing all our paths.
 *
 * @param global_map
 * @param global_map_extent  Extent of global map in device coords.
 * @param stamp
 * @param stamp_extent       Extent of the stamp in device
 *                           coords. The hot pixel is at pos 0, 0.
 * @param point              The point.
 */

static void add_to_global_distortion_map (float complex *global_map,
                                          const cairo_rectangle_int_t *global_map_extent,
                                          const dt_liquify_warp_t *warp,
                                          const float complex *stamp,
                                          const cairo_rectangle_int_t *stamp_extent)
{
  PERF_START ();

  cairo_rectangle_int_t mmext = *stamp_extent;
  mmext.x += (int) round (creal (warp->point));
  mmext.y += (int) round (cimag (warp->point));
  cairo_rectangle_int_t cmmext = mmext;
  cairo_region_t *mmreg = cairo_region_create_rectangle (&mmext);
  cairo_region_intersect_rectangle (mmreg, global_map_extent);
  cairo_region_get_extents (mmreg, &cmmext);
  free (mmreg);

  #pragma omp parallel for schedule (static) default (shared)

  for (int y = cmmext.y; y < cmmext.y + cmmext.height; y++)
  {
    const float complex *srcrow = stamp + ((y - mmext.y) * mmext.width);

    float complex *destrow = global_map +
      ((y - global_map_extent->y) * global_map_extent->width);

    for (int x = cmmext.x; x < cmmext.x + cmmext.width; x++)
    {
      destrow[x - global_map_extent->x] -= srcrow[x - mmext.x];
    }
  }

  PERF_STOP ("add_to_global_distortion_map");
}

/**
 * Applies the global distortion map to the picture.  The distortion
 * map maps points to the position from where the new color of the
 * point should be sampled from.  The distortion map is in relative
 * device coords.
 *
 * @param module
 * @param piece
 * @param in
 * @param out
 * @param roi_in
 * @param roi_out
 * @param map     The global distortion map.
 * @param extent  The extent of the global distortion map.
 */

static void apply_global_distortion_map (struct dt_iop_module_t *module,
                                         dt_dev_pixelpipe_iop_t *piece,
                                         const float *in,
                                         float *out,
                                         const dt_iop_roi_t *roi_in,
                                         const dt_iop_roi_t *roi_out,
                                         const float complex *map,
                                         const cairo_rectangle_int_t *extent)
{
  const int ch = piece->colors;
  const int ch_width = ch * roi_in->width;
  const struct dt_interpolation * const interpolation =
    dt_interpolation_new (DT_INTERPOLATION_USERPREF);

  PERF_START ();

  #pragma omp parallel for schedule (static) default (shared)

  for (int y = extent->y; y < extent->y + extent->height; y++)
  {
    // point inside roi_out ?
    if (y >= roi_out->y && y < roi_out->y + roi_out->height)
    {
      const float complex *row = map + (y - extent->y) * extent->width;
      float* out_sample = out + ((y - roi_out->y) * roi_out->width +
                               extent->x - roi_out->x) * ch;
      for (int x = extent->x; x < extent->x + extent->width; x++)
      {
        if (
          // point inside roi_out ?
          (x >= roi_out->x && x < roi_out->x + roi_out->width) &&
          // point actually warped ?
          (*row != 0))
        {
          dt_interpolation_compute_pixel4c (
            interpolation,
            in,
            out_sample,
            x + creal (*row) - roi_in->x,
            y + cimag (*row) - roi_in->y,
            roi_in->width,
            roi_in->height,
            ch_width);
        }
        ++row;
        out_sample += ch;
      }
    }
  }

  PERF_STOP ("distortion map applied");
}

static double complex * _get_point (const GList *path)
{
  if (path && path->data)
    return &((dt_liquify_path_data_t *) path->data)->point;
  return NULL;
}

static void move_to (cairo_t *cr, double complex pt)
{
  cairo_move_to (cr, creal (pt), cimag (pt));
}

static void line_to (cairo_t *cr, double complex pt)
{
  cairo_line_to (cr, creal (pt), cimag (pt));
}

static void curve_to (cairo_t *cr, double complex pt1, double complex pt2, double complex pt3)
{
  cairo_curve_to (cr, creal (pt1), cimag (pt1), creal (pt2), cimag (pt2), creal (pt3), cimag (pt3));
}

/**
 * Calculate the map extent.
 *
 * @param roi_out
 * @param interpolated  List of warps.
 * @param map_extent    [out] Extent of map.
 */

static void _get_map_extent (const dt_iop_roi_t *roi_out,
                             GList *interpolated,
                             cairo_rectangle_int_t *map_extent)
{
  const cairo_rectangle_int_t roi_out_rect = { roi_out->x, roi_out->y, roi_out->width, roi_out->height };
  cairo_region_t *roi_out_region = cairo_region_create_rectangle (&roi_out_rect);
  cairo_region_t *map_region = cairo_region_create ();

  for (GList *i = interpolated; i != NULL; i = i->next)
  {
    const dt_liquify_warp_t *warp = ((dt_liquify_warp_t *) i->data);
    cairo_rectangle_int_t r;
    compute_round_stamp_extent (&r, warp);
    // add extent if not entirely outside the roi
    if (cairo_region_contains_rectangle (roi_out_region, &r) != CAIRO_REGION_OVERLAP_OUT)
    {
      cairo_region_union_rectangle (map_region, &r);
    }
  }

  // return the paths and the extent of all paths
  cairo_region_get_extents (map_region, map_extent);
  cairo_region_destroy (map_region);
  cairo_region_destroy (roi_out_region);
}

static float complex *build_global_distortion_map (struct dt_iop_module_t *module,
                                                   const dt_dev_pixelpipe_iop_t *piece,
                                                   const dt_iop_roi_t *roi_in,
                                                   const dt_iop_roi_t *roi_out,
                                                   cairo_rectangle_int_t *map_extent)
{
  PRINT_FUNC ();

  debug_params ("unserialized in build_global_distortion_map ()", piece->data);
  GList *paths = unserialize_params (piece->data);
  if (!paths)
    return NULL;
  distort_paths_raw_to_piece (module, piece, roi_in->scale, paths);

  GList *interpolated = interpolate_paths (paths);

  _get_map_extent (roi_out, interpolated, map_extent);

  // allocate distortion map big enough to contain all paths
  debug_rect ("map  extent:", map_extent);
  const int mapsize = map_extent->width * map_extent->height;
  float complex * const map = dt_alloc_align (16, mapsize * sizeof (float complex));
  // for (float *p = map; p < map + mapsize; ++p) *p = 0.0;
  memset (map, 0, mapsize * sizeof (float complex));

  // build map
  for (GList *i = interpolated; i != NULL; i = i->next)
  {
    const dt_liquify_warp_t *warp = ((dt_liquify_warp_t *) i->data);
    float complex *stamp = NULL;
    cairo_rectangle_int_t r;
    build_round_stamp (&stamp, &r, warp);
    add_to_global_distortion_map (map, map_extent, warp, stamp, &r);
    free ((void *) stamp);
  }

  g_list_free_full (interpolated, free);
  free_paths (paths);
  return map;
}


void modify_roi_out (struct dt_iop_module_t *module,
                     struct dt_dev_pixelpipe_iop_t *piece,
                     dt_iop_roi_t *roi_out,
                     const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}

void modify_roi_in (struct dt_iop_module_t *module,
                    struct dt_dev_pixelpipe_iop_t *piece,
                    const dt_iop_roi_t *roi_out,
                    dt_iop_roi_t *roi_in)
{
  PRINT_FUNC ();

  // Because we move pixels, and we may have to sample a pixel from
  // outside roi_in, we need to expand roi_in to contain all our
  // paths.  But we may ignore paths completely outside of roi_out.

  *roi_in = *roi_out;

  debug_params ("unserialized in modify_roi_in ()", piece->data);
  GList *paths = unserialize_params (piece->data);
  if (!paths)
    return;
  distort_paths_raw_to_piece (module, piece, roi_in->scale, paths);

  cairo_rectangle_int_t pipe_rect = {
    0,
    0,
    piece->pipe->iwidth  * roi_in->scale, // FIXME: why is this -1
    piece->pipe->iheight * roi_in->scale  // in spots.c ?
  };
  cairo_rectangle_int_t roi_in_rect = {
    roi_in->x,
    roi_in->y,
    roi_in->width,
    roi_in->height
  };
  cairo_region_t *roi_in_region = cairo_region_create_rectangle (&roi_in_rect);

  // get extent of all paths
  GList *interpolated = interpolate_paths (paths);
  cairo_rectangle_int_t extent;
  _get_map_extent (roi_out, interpolated, &extent);

  debug_rect ("modify_roi_in () extent:", &extent);

  // (eventually) extend roi_in
  cairo_region_union_rectangle (roi_in_region, &extent);
  // and clamp to pipe extent
  cairo_region_intersect_rectangle (roi_in_region, &pipe_rect);

  // write new extent to roi_in
  cairo_region_get_extents (roi_in_region, &roi_in_rect);
  roi_in->x = roi_in_rect.x;
  roi_in->y = roi_in_rect.y;
  roi_in->width  = roi_in_rect.width;
  roi_in->height = roi_in_rect.height;

  // cleanup
  cairo_region_destroy (roi_in_region);
  g_list_free_full (interpolated, free);
  free_paths (paths);
}

void process (struct dt_iop_module_t *module,
              dt_dev_pixelpipe_iop_t *piece,
              const float *in,
              float *out,
              const dt_iop_roi_t *roi_in,
              const dt_iop_roi_t *roi_out)
{
  PRINT_FUNC_ARGS ("size=%d %d", piece->buf_in.width, piece->buf_in.height);
  debug_roi ("roi_in: ", roi_in);
  debug_roi ("roi_out:", roi_out);

  // 1. copy the whole image (we'll change only a small part of it)

  const int ch = piece->colors;
  assert (ch == 4);

  #pragma omp parallel for schedule (static) default (shared)
  for (int i = 0; i < roi_out->height; i++)
  {
    float *destrow = out + (size_t) ch * i * roi_out->width;
    const float *srcrow = in + (size_t) ch * (roi_in->width * (i + roi_out->y - roi_in->y) +
                                              roi_out->x - roi_in->x);
    memcpy (destrow, srcrow, sizeof (float) * ch * roi_out->width);
  }

  // 2. build the distortion map

  cairo_rectangle_int_t map_extent;
  float complex *map = build_global_distortion_map (module, piece, roi_in, roi_out, &map_extent);
  if (map == NULL || map_extent.width == 0 || map_extent.height == 0)
    return;

  // 3. apply the map

  apply_global_distortion_map (module, piece, in, out, roi_in, roi_out, map, &map_extent);
  dt_free_align ((void *) map);
}

#ifdef HAVE_OPENCL

/**
 * Compute lanczos kernel.
 *
 * See: https://en.wikipedia.org/wiki/Lanczos_resampling#Lanczos_kernel
 *
 */

float lanczos (const float a, const float x)
{
  if (fabs (x) >= a) return 0.0f;
  if (fabs (x) < CL_FLT_EPSILON) return 1.0f;

  return
    (a * sin (M_PI * x) * sin (M_PI * x / a))
    /
    (M_PI * M_PI * x * x);
}

/**
 * Compute bicubic kernel.
 *
 * See: https://en.wikipedia.org/wiki/Bicubic_interpolation#Bicubic_convolution_algorithm
 *
 */

float bicubic (const float a, const float x)
{
  const float absx = fabs (x);

  if (absx <= 1)
    return ((a + 2) * absx - (a + 3)) * absx * absx + 1;

  if (absx < 2)
    return ((a * absx - 5 * a) * absx + 8 * a) * absx - 4 * a;

  return 0.0f;
}

typedef struct {
  int size;
  int resolution;
} dt_liquify_kernel_descriptor_t;

typedef cl_mem cl_mem_t;
typedef cl_int cl_int_t;

static cl_int_t apply_global_distortion_map_cl (struct dt_iop_module_t *module,
                                                dt_dev_pixelpipe_iop_t *piece,
                                                const cl_mem_t dev_in,
                                                const cl_mem_t dev_out,
                                                const dt_iop_roi_t *roi_in,
                                                const dt_iop_roi_t *roi_out,
                                                const float complex *map,
                                                const cairo_rectangle_int_t *map_extent)
{
  PERF_START ();

  cl_int_t err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  dt_iop_liquify_global_data_t *gd = (dt_iop_liquify_global_data_t *) module->data;
  const int devid = piece->pipe->devid;

  const struct dt_interpolation* interpolation = dt_interpolation_new (DT_INTERPOLATION_USERPREF);
  dt_liquify_kernel_descriptor_t kdesc = { .size = 0, .resolution = 100 };
  float *k = NULL;

  switch (interpolation->id)
  {
  case DT_INTERPOLATION_BILINEAR:
    kdesc.size = 1;
    kdesc.resolution = 1;
    k = malloc (2 * sizeof (float));
    k[0] = 1.0f;
    k[1] = 0.0f;
    break;
  case DT_INTERPOLATION_BICUBIC:
    kdesc.size = 2;
    k = malloc ((kdesc.size * kdesc.resolution + 1) * sizeof (float));
    for (int i = 0; i <= kdesc.size * kdesc.resolution; ++i)
      k[i] = bicubic (0.5f, (float) i / kdesc.resolution);
    break;
  case DT_INTERPOLATION_LANCZOS2:
    kdesc.size = 2;
    k = malloc ((kdesc.size * kdesc.resolution + 1) * sizeof (float));
    for (int i = 0; i <= kdesc.size * kdesc.resolution; ++i)
      k[i] = lanczos (2, (float) i / kdesc.resolution);
    break;
  case DT_INTERPOLATION_LANCZOS3:
    kdesc.size = 3;
    k = malloc ((kdesc.size * kdesc.resolution + 1) * sizeof (float));
    for (int i = 0; i <= kdesc.size * kdesc.resolution; ++i)
      k[i] = lanczos (3, (float) i / kdesc.resolution);
    break;
  default:
    return FALSE;
  }

  cl_mem_t dev_roi_in = dt_opencl_copy_host_to_device_constant (
    devid, sizeof (dt_iop_roi_t), (void *) roi_in);
  if (dev_roi_in == NULL) goto error;

  cl_mem_t dev_roi_out = dt_opencl_copy_host_to_device_constant (
    devid, sizeof (dt_iop_roi_t), (void *) roi_out);
  if (dev_roi_out == NULL) goto error;

  cl_mem_t dev_map = dt_opencl_copy_host_to_device_constant (
    devid, map_extent->width * map_extent->height * sizeof (float complex), (void *) map);
  if (dev_map == NULL) goto error;

  cl_mem_t dev_map_extent = dt_opencl_copy_host_to_device_constant (
    devid, sizeof (cairo_rectangle_int_t), (void *) map_extent);
  if (dev_map_extent == NULL) goto error;

  cl_mem_t dev_kdesc = dt_opencl_copy_host_to_device_constant (
    devid, sizeof (dt_liquify_kernel_descriptor_t), (void *) &kdesc);
  if (dev_kdesc == NULL) goto error;

  cl_mem_t dev_kernel = dt_opencl_copy_host_to_device_constant (
    devid, (kdesc.size * kdesc.resolution  + 1) * sizeof (float), (void *) k);
  if (dev_kernel == NULL) goto error;

  dt_opencl_set_kernel_arg (devid, gd->warp_kernel, 0, sizeof (cl_mem), &dev_in);
  dt_opencl_set_kernel_arg (devid, gd->warp_kernel, 1, sizeof (cl_mem), &dev_out);
  dt_opencl_set_kernel_arg (devid, gd->warp_kernel, 2, sizeof (cl_mem), &dev_roi_in);
  dt_opencl_set_kernel_arg (devid, gd->warp_kernel, 3, sizeof (cl_mem), &dev_roi_out);
  dt_opencl_set_kernel_arg (devid, gd->warp_kernel, 4, sizeof (cl_mem), &dev_map);
  dt_opencl_set_kernel_arg (devid, gd->warp_kernel, 5, sizeof (cl_mem), &dev_map_extent);

  dt_opencl_set_kernel_arg (devid, gd->warp_kernel, 6, sizeof (cl_mem), &dev_kdesc);
  dt_opencl_set_kernel_arg (devid, gd->warp_kernel, 7, sizeof (cl_mem), &dev_kernel);

  const size_t sizes[] = { ROUNDUPWD (map_extent->width), ROUNDUPHT (map_extent->height) };
  err = dt_opencl_enqueue_kernel_2d (devid, gd->warp_kernel, sizes);

error:

  if (dev_kernel    ) dt_opencl_release_mem_object (dev_kernel);
  if (dev_kdesc     ) dt_opencl_release_mem_object (dev_kdesc);
  if (dev_map_extent) dt_opencl_release_mem_object (dev_map_extent);
  if (dev_map       ) dt_opencl_release_mem_object (dev_map);
  if (dev_roi_out   ) dt_opencl_release_mem_object (dev_roi_out);
  if (dev_roi_in    ) dt_opencl_release_mem_object (dev_roi_in);
  if (k             ) free (k);

  PERF_STOP ("opencl distortion map enqueued");
  return err;
}

int process_cl (struct dt_iop_module_t *module,
                dt_dev_pixelpipe_iop_t *piece,
                const cl_mem_t dev_in,
                const cl_mem_t dev_out,
                const dt_iop_roi_t *roi_in,
                const dt_iop_roi_t *roi_out)
{
  PRINT_FUNC_ARGS ("size=%d %d", piece->buf_in.width, piece->buf_in.height);
  debug_roi ("roi_in: ", roi_in);
  debug_roi ("roi_out:", roi_out);

  cl_int_t err = -999;
  const int devid = piece->pipe->devid;

  // 1. copy the whole image (we'll change only a small part of it)

  {
    size_t src[]    = { roi_out->x - roi_in->x, roi_out->y - roi_in->y, 0 };
    size_t dest[]   = { 0, 0, 0 };
    size_t extent[] = { roi_out->width, roi_out->height, 1 };
    err = dt_opencl_enqueue_copy_image (devid, dev_in, dev_out, src, dest, extent);
    if (err != CL_SUCCESS) goto error;
  }

  // 2. build the distortion map

  cairo_rectangle_int_t map_extent;
  const float complex *map = build_global_distortion_map (module, piece, roi_in, roi_out, &map_extent);
  if (map == NULL || map_extent.width == 0 || map_extent.height == 0)
    return TRUE;

  // 3. apply the map

  err = apply_global_distortion_map_cl (module, piece, dev_in, dev_out, roi_in, roi_out, map, &map_extent);
  dt_free_align ((void *) map);
  if (err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print (DT_DEBUG_OPENCL, "[opencl_liquify] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}

#endif

/* init, cleanup, commit to pipeline */

// init_global -> init -> gui_init -> init_pipe
// init_global -> init -> init_pipe

void init_global (dt_iop_module_so_t *module)
{
  // called once at startup
  PRINT_FUNC ();

  const int program = 16; // from programs.conf
  dt_iop_liquify_global_data_t *gd = (dt_iop_liquify_global_data_t *) malloc (sizeof (dt_iop_liquify_global_data_t));
  module->data = gd;
  gd->warp_kernel = dt_opencl_create_kernel (program, "warp_kernel");

  // some constants for SSE2
  struct __attribute__ ((aligned (16))) {
    float three[4];
    float threehalfs[4];
    float half[4];
  } _mm_const = { {3.0f, 3.0f, 3.0f, 3.0f},
                  {1.5f, 1.5f, 1.5f, 1.5f},
                  {0.5f, 0.5f, 0.5f, 0.5f} };

  _mm_constants.three      = _mm_load_ps (_mm_const.three);
  _mm_constants.threehalfs = _mm_load_ps (_mm_const.threehalfs);
  _mm_constants.half       = _mm_load_ps (_mm_const.half);
}

void cleanup_global (dt_iop_module_so_t *module)
{
  // called once at shutdown
  PRINT_FUNC ();

  dt_iop_liquify_global_data_t *gd = (dt_iop_liquify_global_data_t *) module->data;
  dt_opencl_free_kernel (gd->warp_kernel);
  free (module->data);
  module->data = NULL;
}

void init (dt_iop_module_t *module)
{
  PRINT_FUNC ();

  const size_t size = 16 * 1024;

  // our module is disabled by default
  module->default_enabled = 0;
  module->priority = 200; // module order created by iop_dependencies.py, do not edit!
  module->params_size = size;
  module->gui_data = NULL;
  // init defaults:
  dt_iop_liquify_params_t tmp =
  {
    // blob size
    0,
    // blob_version
    1,
  };

  module->params = calloc (1, size);
  memcpy (module->params, &tmp, sizeof tmp);

  module->default_params = calloc (1, size);
  memcpy (module->default_params, &tmp, sizeof tmp);
}

void cleanup (dt_iop_module_t *module)
{
  PRINT_FUNC ();

  free (module->params);
  module->params = NULL;
}

void init_pipe (struct dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  PRINT_FUNC ();

  piece->data = malloc (module->params_size);
  module->commit_params (module, module->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  PRINT_FUNC ();

  free (piece->data);
  piece->data = NULL;
}

/** commit is the synch point between core and gui, so it copies params to pipe data. */

void commit_params (struct dt_iop_module_t *module,
                    dt_iop_params_t *params,
                    dt_dev_pixelpipe_t *pipe,
                    dt_dev_pixelpipe_iop_t *piece)
{
  PRINT_FUNC ();

  memcpy (piece->data, params, module->params_size);
  // piece->process_cl_ready = 0; // disable OpenCL for debugging
}


/******************************************************************************/
/* Gui code.                                                                  */
/******************************************************************************/

/**
 * Calculate the dot product of 2 vectors.
 *
 * @param p0  Vector 1.
 * @param p1  Vector 2.
 *
 * @return    The dot product.
 */

static double cdot (const double complex p0, const double complex p1)
{
#ifdef FP_FAST_FMA
  return fma (creal (p0), creal (p1), cimag (p0) * cimag (p1));
#else
  return creal (p0) * creal (p1) + cimag (p0) * cimag (p1);
#endif
}

static void draw_rectangle (cairo_t *cr, const double complex pt, const double theta, const double size)
{
  const double x = creal (pt), y = cimag (pt);
  cairo_save (cr);
  cairo_translate (cr, x, y);
  cairo_rotate (cr, theta);
  cairo_rectangle (cr, -size / 2.0, -size / 2.0, size, size);
  cairo_restore (cr);
}

static void draw_triangle (cairo_t *cr, const double complex pt, const double theta, const double size)
{
  const double x = creal (pt), y = cimag (pt);
  cairo_save (cr);
  cairo_translate (cr, x, y);
  cairo_rotate (cr, theta);
  cairo_move_to (cr, -size, -size / 2.0);
  cairo_line_to (cr, 0,     0          );
  cairo_line_to (cr, -size, +size / 2.0);
  cairo_close_path (cr);
  cairo_restore (cr);
}

static void draw_circle (cairo_t *cr, const double complex pt, const double diameter)
{
  const double x = creal (pt), y = cimag (pt);
  cairo_save (cr);
  cairo_new_sub_path (cr);
  cairo_arc (cr, x, y, diameter / 2.0, 0, 2 * M_PI);
  cairo_restore (cr);
}

static void set_source_rgba (cairo_t *cr, dt_liquify_rgba_t rgba)
{
  cairo_set_source_rgba (cr, rgba.red, rgba.green, rgba.blue, rgba.alpha);
}

static double get_ui_width (const double scale, const dt_liquify_ui_width_enum_t w)
{
  assert (w >= 0 && w < DT_LIQUIFY_UI_WIDTH_LAST);
  return scale * DT_PIXEL_APPLY_DPI (dt_liquify_ui_widths[w]);
}

#define GET_UI_WIDTH(a) (get_ui_width (scale, DT_LIQUIFY_UI_WIDTH_##a))

static void set_line_width (cairo_t *cr, double scale, dt_liquify_ui_width_enum_t w)
{
  const double width = get_ui_width (scale, w);
  cairo_set_line_width (cr, width);
}

static bool detect_drag (const dt_iop_liquify_gui_data_t *g, const double scale, const double complex pt)
{
  // g->last_button1_pressed_pos is valid only while BUTTON1 is down
  return g->last_button1_pressed_pos != -1.0 &&
    cabs (pt - g->last_button1_pressed_pos) >= GET_UI_WIDTH (MIN_DRAG);
}

static void hint (const char *msg)
{
  dt_control_hinter_message (darktable.control, msg);
}

static void update_warp_count (const dt_iop_liquify_gui_data_t *g)
{
  const guint nb = g_list_length (g->paths);
  char str[6];
  snprintf (str, sizeof (str), "%d", nb);
  gtk_label_set_text (g->label, str);
}

static GList *interpolate_paths (GList *paths)
{
  GList *l = NULL;

  for (GList *i = paths; i != NULL; i = i->next)
  {
    for (GList *j = i->data; j != NULL; j = j->next)
    {
      const dt_liquify_path_data_t *data = (dt_liquify_path_data_t *) j->data;

      const double complex *p2 = &data->point;
      const dt_liquify_warp_t *warp2 = &data->warp;

      if (data->header.type == DT_LIQUIFY_PATH_MOVE_TO_V1)
      {
        if (!j->next)
        {
          dt_liquify_warp_t *w = malloc (sizeof (dt_liquify_warp_t));
          *w = *warp2;
          l = g_list_append (l, w);
        }
        continue;
      }

      const dt_liquify_warp_t *warp1 = &((dt_liquify_path_data_t *) j->prev->data)->warp;
      const double complex *p1 = _get_point (j->prev);

      if (data->header.type == DT_LIQUIFY_PATH_LINE_TO_V1)
      {
        const double total_length = cabs (*p1 - *p2);
        double arc_length = 0.0;
        while (arc_length < total_length)
        {
          dt_liquify_warp_t *w = malloc (sizeof (dt_liquify_warp_t));
          const double t = arc_length / total_length;
          const double complex pt = cmix (*p1, *p2, t);
          mix_warps (w, warp1, warp2, pt, t);
          w->strength = cmix (w->point, w->strength, STAMP_RELOCATION);
          arc_length += cabs (w->radius - w->point) * STAMP_RELOCATION;
          l = g_list_append (l, w);
        }
        continue;
      }

      if (data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
      {
        float complex *buffer = malloc (INTERPOLATION_POINTS * sizeof (float complex));
        interpolate_cubic_bezier (*p1,
                                  data->curve_to_v1.ctrl1,
                                  data->curve_to_v1.ctrl2,
                                  *p2,
                                  buffer,
                                  INTERPOLATION_POINTS);
        const double total_length = get_arc_length (buffer, INTERPOLATION_POINTS);
        double arc_length = 0.0;
        restart_cookie_t restart = { 1, 0.0 };

        while (arc_length < total_length)
        {
          dt_liquify_warp_t *w = malloc (sizeof (dt_liquify_warp_t));
          const double complex pt = point_at_arc_length (buffer, INTERPOLATION_POINTS, (const double)arc_length, &restart);
          mix_warps (w, warp1, warp2, pt, arc_length / total_length);
          w->strength = cmix (w->point, w->strength, STAMP_RELOCATION);
          arc_length += cabs (w->radius - w->point) * STAMP_RELOCATION;
          l = g_list_append (l, w);
        }
        free ((void *) buffer);
        continue;
      }
    }
  }
  return l;
}

#define STROKE_TEST \
  if (do_hit_test) { if (cairo_in_stroke (cr, creal (*pt), cimag (*pt))) goto hit; continue; }

#define FILL_TEST \
  if (do_hit_test) { if (cairo_in_fill (cr, creal (*pt), cimag (*pt)) || cairo_in_stroke (cr, creal (*pt), cimag (*pt))) goto hit; continue; }

#define FG_COLOR  set_source_rgba (cr, fg_color);
#define BG_COLOR  set_source_rgba (cr, bg_color);
#define THINLINE  set_line_width  (cr, scale, DT_LIQUIFY_UI_WIDTH_THINLINE);
#define THICKLINE set_line_width  (cr, scale, DT_LIQUIFY_UI_WIDTH_THICKLINE);

static dt_liquify_hit_t _draw_paths (const struct dt_iop_module_t *module,
                                     cairo_t *cr,
                                     const double scale,
                                     GList *paths,
                                     GList *layers,
                                     const double complex *pt)
{
  // dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;
  dt_liquify_hit_t hit = NOWHERE;
  const bool do_hit_test = pt != NULL;

  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

  GList *interpolated = do_hit_test ? NULL : interpolate_paths (paths);

  for (GList *l = layers; l != NULL; l = l->next)
  {
    const dt_liquify_layer_enum_t layer = (dt_liquify_layer_enum_t) GPOINTER_TO_INT (l->data);
    dt_liquify_rgba_t fg_color = dt_liquify_layers[layer].fg;
    dt_liquify_rgba_t bg_color = dt_liquify_layers[layer].bg;

    if (do_hit_test && ((dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_HIT_TEST) == 0))
      continue;

    hit.layer = layer;

    if (dt_liquify_layers[layer].opacity < 1.0)
        cairo_push_group (cr);

    for (GList *i = paths; i != NULL; i = i->next)
    {
      // render one path
      for (GList *j = i->data; j != NULL; j = j->next)
      {
        dt_liquify_path_data_t *data = (dt_liquify_path_data_t *) j->data;
        const dt_liquify_path_data_t *prev = j->prev ? (dt_liquify_path_data_t *) j->prev->data : NULL;

        hit.elem = data;

        if ((dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED)
            && !data->header.selected)
          continue;

        if ((dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_PREV_SELECTED)
            && (!prev || !prev->header.selected))
          continue;

        fg_color = dt_liquify_layers[layer].fg;
        bg_color = dt_liquify_layers[layer].bg;

        if (data->header.selected == layer)
          fg_color = DT_LIQUIFY_COLOR_SELECTED;

        if (data->header.hovered == dt_liquify_layers[layer].hover_master)
          fg_color = DT_LIQUIFY_COLOR_HOVER;

        cairo_new_path (cr);

        if (data->header.type == DT_LIQUIFY_PATH_CLOSE_PATH_V1)
        {
          cairo_close_path (cr);
          continue;
        }

        const double complex point = *_get_point (j);

        if (data->header.type == DT_LIQUIFY_PATH_MOVE_TO_V1)
          move_to (cr, point);

        if (layer == DT_LIQUIFY_LAYER_RADIUS)
        {
          for (GList *i = interpolated; i != NULL; i = i->next)
          {
            const dt_liquify_warp_t *pwarp = ((dt_liquify_warp_t *) i->data);
            draw_circle (cr, pwarp->point, 2.0f * cabs (pwarp->radius - pwarp->point));
          }
          FG_COLOR;
          cairo_fill (cr);
        }

        if (layer == DT_LIQUIFY_LAYER_HARDNESS1)
        {
          for (GList *i = interpolated; i != NULL; i = i->next)
          {
            const dt_liquify_warp_t *pwarp = ((dt_liquify_warp_t *) i->data);
            draw_circle (cr, pwarp->point, 2.0f * cabs (pwarp->radius - pwarp->point) * pwarp->control1);
          }
          FG_COLOR;
          cairo_fill (cr);
        }

        if (layer == DT_LIQUIFY_LAYER_HARDNESS2)
        {
          for (GList *i = interpolated; i != NULL; i = i->next)
          {
            const dt_liquify_warp_t *pwarp = ((dt_liquify_warp_t *) i->data);
            draw_circle (cr, pwarp->point, 2.0f * cabs (pwarp->radius - pwarp->point) * pwarp->control2);
          }
          FG_COLOR;
          cairo_fill (cr);
        }

        if (layer == DT_LIQUIFY_LAYER_WARPS)
        {
          THINLINE; FG_COLOR;
          for (GList *i = interpolated; i != NULL; i = i->next)
          {
            const dt_liquify_warp_t *pwarp = ((dt_liquify_warp_t *) i->data);
            move_to (cr, pwarp->point);
            line_to (cr, pwarp->strength);
          }
          cairo_stroke (cr);

          for (GList *i = interpolated; i != NULL; i = i->next)
          {
            const dt_liquify_warp_t *pwarp = ((dt_liquify_warp_t *) i->data);
            const double rot = get_rot (pwarp->type);
            draw_circle   (cr, pwarp->point, GET_UI_WIDTH (GIZMO_SMALL));
            draw_triangle (cr, pwarp->strength,
                           carg (pwarp->strength - pwarp->point) + rot,
                           GET_UI_WIDTH (GIZMO_SMALL));
          }
          BG_COLOR;
          cairo_fill_preserve (cr);
          FG_COLOR;
          cairo_stroke (cr);
        }

        if (layer == DT_LIQUIFY_LAYER_PATH)
        {
          if ((data->header.type == DT_LIQUIFY_PATH_LINE_TO_V1)
              || (data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1))
          {
            assert (prev);
            move_to (cr, *_get_point (j->prev));
            if (data->header.type == DT_LIQUIFY_PATH_LINE_TO_V1)
              line_to (cr, point);
            if (data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1) {
              curve_to (cr, data->curve_to_v1.ctrl1, data->curve_to_v1.ctrl2, point);
            }
            THICKLINE; FG_COLOR;
            STROKE_TEST;
            cairo_stroke_preserve (cr);
            THINLINE; BG_COLOR;
            cairo_stroke (cr);
          }
        }

        if (layer == DT_LIQUIFY_LAYER_CENTERPOINT)
        {
          if (data->header.type == DT_LIQUIFY_PATH_MOVE_TO_V1
              || data->header.type == DT_LIQUIFY_PATH_LINE_TO_V1
              || data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
          {
            const double w = GET_UI_WIDTH (GIZMO);
            switch (data->header.node_type)
            {
            case DT_LIQUIFY_NODE_TYPE_CUSP:
              draw_triangle (cr, point - w / 2.0 * I, -M_PI / 2.0, w);
              break;
            case DT_LIQUIFY_NODE_TYPE_SMOOTH:
              draw_rectangle (cr, point, M_PI / 4.0, w);
              break;
            case DT_LIQUIFY_NODE_TYPE_SYMMETRICAL:
              draw_rectangle (cr, point, 0, w);
              break;
            case DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH:
              draw_circle (cr, point, w);
              break;
            default:
              break;
            }
            THINLINE; BG_COLOR;
            FILL_TEST;
            cairo_fill_preserve (cr);
            FG_COLOR;
            cairo_stroke (cr);
          }
        }

        if (data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
        {
          if (layer == DT_LIQUIFY_LAYER_CTRLPOINT1_HANDLE &&
              !(prev && prev->header.node_type == DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH))
          {
            THINLINE; FG_COLOR;
            move_to (cr, prev->point);
            line_to (cr, data->curve_to_v1.ctrl1);
            cairo_stroke (cr);
          }
          if (layer == DT_LIQUIFY_LAYER_CTRLPOINT2_HANDLE &&
              data->header.node_type != DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH)
          {
            THINLINE; FG_COLOR;
            move_to (cr, data->point);
            line_to (cr, data->curve_to_v1.ctrl2);
            cairo_stroke (cr);
          }
          if (layer == DT_LIQUIFY_LAYER_CTRLPOINT1 &&
              !(prev && prev->header.node_type == DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH))
          {
            THINLINE; BG_COLOR;
            draw_circle (cr, data->curve_to_v1.ctrl1, GET_UI_WIDTH (GIZMO_SMALL));
            FILL_TEST;
            cairo_fill_preserve (cr);
            FG_COLOR;
            cairo_stroke (cr);
          }
          if (layer == DT_LIQUIFY_LAYER_CTRLPOINT2 &&
              data->header.node_type != DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH)
          {
            THINLINE; BG_COLOR;
            draw_circle (cr, data->curve_to_v1.ctrl2, GET_UI_WIDTH (GIZMO_SMALL));
            FILL_TEST;
            cairo_fill_preserve (cr);
            FG_COLOR;
            cairo_stroke (cr);
          }
        }

        const dt_liquify_warp_t *warp  = &data->warp;

        if (layer == DT_LIQUIFY_LAYER_RADIUSPOINT_HANDLE)
        {
          draw_circle (cr, point, 2.0 * cabs (warp->radius - point));
          THICKLINE; FG_COLOR;
          cairo_stroke_preserve (cr);
          THINLINE; BG_COLOR;
          cairo_stroke (cr);
        }

        if (layer == DT_LIQUIFY_LAYER_RADIUSPOINT)
        {
          THINLINE; BG_COLOR;
          draw_circle (cr, warp->radius, GET_UI_WIDTH (GIZMO_SMALL));
          FILL_TEST;
          cairo_fill_preserve (cr);
          FG_COLOR;
          cairo_stroke (cr);
        }

        if (layer == DT_LIQUIFY_LAYER_HARDNESSPOINT1_HANDLE)
        {
          draw_circle (cr, point, 2.0 * cabs (warp->radius - point) * warp->control1);
          THICKLINE; FG_COLOR;
          cairo_stroke_preserve (cr);
          THINLINE; BG_COLOR;
          cairo_stroke (cr);
        }

        if (layer == DT_LIQUIFY_LAYER_HARDNESSPOINT2_HANDLE)
        {
          draw_circle (cr, point, 2.0 * cabs (warp->radius - point) * warp->control2);
          THICKLINE; FG_COLOR;
          cairo_stroke_preserve (cr);
          THINLINE; BG_COLOR;
          cairo_stroke (cr);
        }

        if (layer == DT_LIQUIFY_LAYER_HARDNESSPOINT1)
        {
          draw_triangle (cr, cmix (point, warp->radius, warp->control1),
                         carg (warp->radius - point),
                         GET_UI_WIDTH (GIZMO_SMALL));
          THINLINE; BG_COLOR;
          FILL_TEST;
          cairo_fill_preserve (cr);
          FG_COLOR;
          cairo_stroke (cr);
        }

        if (layer == DT_LIQUIFY_LAYER_HARDNESSPOINT2)
        {
          draw_triangle (cr, cmix (point, warp->radius, warp->control2),
                         carg (- (warp->radius - point)),
                         GET_UI_WIDTH (GIZMO_SMALL));
          THINLINE; BG_COLOR;
          FILL_TEST;
          cairo_fill_preserve (cr);
          FG_COLOR;
          cairo_stroke (cr);
        }

        if (layer == DT_LIQUIFY_LAYER_STRENGTHPOINT_HANDLE)
        {
          move_to (cr, point);
          if (warp->type == DT_LIQUIFY_WARP_TYPE_LINEAR)
            line_to (cr, cmix (point, warp->strength, 1.0 - 0.5 *
                               (GET_UI_WIDTH (GIZMO_SMALL) /
                                cabs (warp->strength - point))));
          else
            draw_circle (cr, point, 2.0 * cabs (warp->strength - warp->point));
          THICKLINE; FG_COLOR;
          cairo_stroke_preserve (cr);
          THINLINE; BG_COLOR;
          cairo_stroke (cr);
        }

        if (layer == DT_LIQUIFY_LAYER_STRENGTHPOINT)
        {
          const double rot = get_rot (warp->type);
          draw_triangle (cr, warp->strength,
                         carg (warp->strength - warp->point) + rot,
                         GET_UI_WIDTH (GIZMO_SMALL));
          THINLINE; BG_COLOR;
          FILL_TEST;
          cairo_fill_preserve (cr);
          FG_COLOR;
          cairo_stroke (cr);
        }
      }
    }

    if (dt_liquify_layers[layer].opacity < 1.0)
    {
      cairo_pop_group_to_source (cr);
      cairo_paint_with_alpha (cr, dt_liquify_layers[layer].opacity);
    }
  }

  g_list_free_full (interpolated, free);
  return NOWHERE;

hit:
  g_list_free_full (interpolated, free);
  cairo_new_path (cr); // otherwise a successful hit test would leave the path behind
  return hit;
}

static void draw_paths (struct dt_iop_module_t *module, cairo_t *cr, double scale, GList *paths)
{
  const dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;
  GList *layers = NULL;

  for (dt_liquify_layer_enum_t layer = 0; layer < DT_LIQUIFY_LAYER_LAST; ++layer)
  {
    if (gtk_toggle_button_get_active (g->btn_point_tool)
        && (dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_POINT_TOOL))
      layers = g_list_append (layers, GINT_TO_POINTER (layer));
    if (gtk_toggle_button_get_active (g->btn_line_tool)
        && (dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_LINE_TOOL))
      layers = g_list_append (layers, GINT_TO_POINTER (layer));
    if (gtk_toggle_button_get_active (g->btn_curve_tool)
        && (dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_CURVE_TOOL))
      layers = g_list_append (layers, GINT_TO_POINTER (layer));
    if (gtk_toggle_button_get_active (g->btn_node_tool)
        && (dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_NODE_TOOL))
      layers = g_list_append (layers, GINT_TO_POINTER (layer));
  }

  _draw_paths (module, cr, scale, paths, layers, NULL);

  g_list_free (layers);
}

static dt_liquify_hit_t hit_test_paths (struct dt_iop_module_t *module,
                                        double scale,
                                        cairo_t *cr,
                                        GList *paths,
                                        double complex pt)
{
  dt_liquify_hit_t hit = NOWHERE;
  GList *layers = NULL;

  for (dt_liquify_layer_enum_t layer = 0; layer < DT_LIQUIFY_LAYER_LAST; ++layer)
  {
    if (dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_HIT_TEST)
      layers = g_list_append (layers, GINT_TO_POINTER (layer));
  }

  layers = g_list_reverse (layers);
  hit = _draw_paths (module, cr, scale, paths, layers, &pt);
  g_list_free (layers);
  return hit;
}

/**
 * Split a cubic bezier at t into two cubic beziers.
 *
 * @param p0
 * @param p1  InOut
 * @param p2  InOut
 * @param p3  InOut
 * @param t   Parameter t in the range 0.0 .. 1.0.
 */

static void casteljau (const double complex *p0, double complex *p1, double complex *p2, double complex *p3, const double t)
{
  const double complex p01 = *p0 + (*p1 - *p0) * t;
  const double complex p12 = *p1 + (*p2 - *p1) * t;
  const double complex p23 = *p2 + (*p3 - *p2) * t;

  const double complex p012 = p01 + (p12 - p01) * t;
  const double complex p123 = p12 + (p23 - p12) * t;

  const double complex p0123 = p012 + (p123 - p012) * t;

  *p1 = p01;
  *p2 = p012;
  *p3 = p0123;
}

/**
 * Find the nearest point on a cubic bezier curve.
 *
 * Return the curve parameter t of the point on a cubic bezier curve
 * that is nearest to another arbitrary point.  Uses interpolation.
 *
 * FIXME: Implement a faster method, see:
 * http://tog.acm.org/resources/GraphicsGems/gems/NearestPoint.c
 *
 * @param p0  The cubic bezier.
 * @param p1
 * @param p2
 * @param p3
 * @param x   An arbitrary point.
 * @param n   No. of interpolations.
 *
 * @return    Curve parameter t.
 */

static double find_nearest_on_curve_t (const double complex p0,
                                       const double complex p1,
                                       const double complex p2,
                                       const double complex p3,
                                       const double complex x,
                                       const int n)
{
  double min_t = 0, min_dist = cabs (x - p0);

  for (int i = 0; i < n; i++)
  {
    const float t = (1.0 * i) / n;
    const float t1 = 1.0 - t;
    const float complex ip =
          t1 * t1 * t1 * p0 +
      3 * t1 * t1 * t  * p1 +
      3 * t1 * t  * t  * p2 +
          t  * t  * t  * p3;

    const double dist = cabs (x - ip);
    if (dist < min_dist)
    {
      min_dist = dist;
      min_t = t;
    }
  }
  return min_t;
}

/**
 * Find the nearest point on a line.
 *
 * Return the line parameter t of the point on a line that is nearest
 * to another arbitrary point.
 *
 * @param p0  Line.
 * @param p1  Line.
 * @param x   Arbitrary point.
 *
 * @return    Parameter t. (Is in the range 0.0 .. 1.0 if the projection
 *            intersects the line.)
 */

static double find_nearest_on_line_t (const double complex p0, const double complex p1, const double complex x)
{
  // scalar projection
  const double b     = cabs (p1 - p0);           // |b|
  const double dotab = cdot (x - p0, p1 - p0);   // |a| * |b| * cos(phi)
  return dotab / (b * b);                        // |a| / |b| * cos(phi)
}

/**
 * Smooth a bezier spline through prescribed points.
 *
 * Smooth a bezier spline through prescribed points by solving a
 * linear system.  First we build a tridiagonal matrix and then we
 * solve it using the Thomas algorithm.  (FIXME: A tridiagonal matrix
 * is easy to solve in O(n) but you cannot write a closed path as a
 * tridiagonal.  To solve closed paths we will have to use a different
 * solver. Use the GSL?)
 *
 * Here is an article that explains the math:
 * http://www.particleincell.com/blog/2012/bezier-splines/ Basically
 * we find all the ctrl1 points when we solve the linear system, then
 * we calculate each ctrl2 from the ctrl1.
 *
 * We build the linear system choosing for each segment of the path an
 * equation among following 9 equations.  "Straight" is a path that
 * goes straight in to the knot (2nd derivative == 0 at the knot).
 * "Smooth" means a path that goes smoothly through the knot, makes no
 * corner and curves the same amount just before and just after the
 * knot (1st and 2nd derivatives are constant around the knot.)
 * "Keep" means to keep the control point as the user set it.
 *
 *      start     end of path
 *
 *   1: straight  smooth
 *   2: smooth    smooth
 *   3: smooth    straight
 *   4: keep      smooth
 *   5: keep      keep
 *   6: smooth    keep
 *   7: keep      straight
 *   8: straight  straight  (yields a line)
 *   9: straight  keep
 *
 * The equations are (close your eyes):
 *
 * \f{eqnarray}{
 *                2P_{1,i} + P_{1,i+1} &=&  K_i + 2K_{i+1}  \eqno(1) \\
 *    P_{1,i-1} + 4P_{1,i} + P_{1,i+1} &=& 4K_i + 2K_{i+1}  \eqno(2) \\
 *   2P_{1,i-1} + 7P_{1,i}             &=& 8K_i +  K_{i+1}  \eqno(3) \\
 *                 P_{1,i}             &=& C1_i             \eqno(4) \\
 *                 P_{1,i}             &=& C1_i             \eqno(5) \\
 *    P_{1,i-1} + 4P_{1,i}             &=& C2_i + 4K_i      \eqno(6) \\
 *                 P_{1,i}             &=& C1_i             \eqno(7) \\
 *                3P_{1,i}             &=& 2K_i +  K_{i+1}  \eqno(8) \\
 *                2P_{1,i}             &=&  K_i +  C2_i     \eqno(9)
 * \f}
 *
 * Some of these are the same and differ only in the way we calculate
 * c2. (You may open your eyes again.)
 *
 * @param n         Number of knots.
 * @param k         Array of n knots.
 * @param c1        Array of n-1 control points. InOut.
 * @param c2        Array of n-1 control points. InOut.
 * @param equation  Array of n-1 equation numbers.
 */

static void smooth_path_linsys (size_t n,
                                const double complex *k,
                                double complex *c1,
                                double complex *c2,
                                const int *equation)
{
  --n;
  double *a = malloc (n * sizeof (double)); // subdiagonal
  double *b = malloc (n * sizeof (double)); // main diagonal
  double *c = malloc (n * sizeof (double)); // superdiagonal
  double complex *d = malloc (n * sizeof (double complex)); // right hand side

  // Build the tridiagonal matrix.

  for (int i = 0; i < n; i++)
  {
    switch (equation[i])
    {
    #define ABCD(A,B,C,D) { { a[i] = A; b[i] = B; c[i] = C; d[i] = D; continue; } }
    case 1:  ABCD (0, 2, 1,       k[i] + 2 * k[i+1]   ); break;
    case 2:  ABCD (1, 4, 1,   4 * k[i] + 2 * k[i+1]   ); break;
    case 3:  ABCD (2, 7, 0,   8 * k[i] +     k[i+1]   ); break;
    case 4:  ABCD (0, 1, 0,                  c1[i]    ); break;
    case 5:  ABCD (0, 1, 0,                  c1[i]    ); break;
    case 6:  ABCD (1, 4, 0,   4 * k[i] +     c2[i]    ); break;
    case 7:  ABCD (0, 1, 0,                  c1[i]    ); break;
    case 8:  ABCD (0, 3, 0,   2 * k[i] +     k[i+1]   ); break;
    case 9:  ABCD (0, 2, 0,       k[i] +     c2[i]    ); break;
    #undef ABCD
    }
  }

  /* PRINT ("--- matrix ---\n");
  for (int i = 0; i < n; i++)
  PRINT ("(%d) %lf %lf %lf (%lf %lf)\n", equation[i], a[i], b[i], c[i], creal (d[i]), cimag (d[i])); */

  // Solve with the Thomas algorithm to compute c1's.  See:
  // http://en.wikipedia.org/wiki/Tridiagonal_matrix_algorithm

  for (int i = 1; i < n; i++)
  {
    const double m = a[i] / b[i-1];
    b[i] = b[i] - m * c[i-1];
    d[i] = d[i] - m * d[i-1];
  }

  c1[n-1] = d[n-1] / b[n-1];
  for (int i = n - 2; i >= 0; i--)
    c1[i] = (d[i] - c[i] * c1[i+1]) / b[i];

  // Now compute the c2's.

  for (int i = 0; i < n; i++)
  {
    switch (equation[i])
    {
    // keep end: c2 does not change
    case 5:
    case 6:
    case 9:  break;

    // straight end: put c2[i] halfway between c1[i] and k[i+1]
    case 3:
    case 7:
    case 8:  c2[i] = (c1[i] + k[i+1]) / 2;  break;

    // smooth end: c2 and c1 are symmetrical around the knot
    default: c2[i] = 2 * k[i+1] - c1[i+1];
    }
  }

  free (a);
  free (b);
  free (c);
  free (d);
}

static void smooth_paths_linsys (dt_iop_liquify_gui_data_t *g)
{
  for (GList *i = g->paths; i != NULL; i = i->next)
  {
    const size_t n = g_list_length (i->data);
    if (n < 2)
      continue;
    double complex *pt   = calloc (n, sizeof (double complex));
    double complex *c1   = calloc (n, sizeof (double complex));
    double complex *c2   = calloc (n, sizeof (double complex));
    int *eqn             = calloc (n, sizeof (int));
    size_t k = 0;

    for (GList *j = i->data; j != NULL; j = j->next)
    {
      const dt_liquify_curve_to_v1_t *d = (dt_liquify_curve_to_v1_t *) j->data;
      const dt_liquify_curve_to_v1_t *p = j->prev ? (dt_liquify_curve_to_v1_t *) j->prev->data : NULL;
      const dt_liquify_curve_to_v1_t *n = j->next ? (dt_liquify_curve_to_v1_t *) j->next->data : NULL;
      const dt_liquify_curve_to_v1_t *nn = j->next && j->next->next ?
        (dt_liquify_curve_to_v1_t *) j->next->next->data : NULL;

      pt[k] = *_get_point (j);
      if (d->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
      {
        c1[k-1] = d->ctrl1;
        c2[k-1] = d->ctrl2;
      }

      const int autosmooth      = d->header.node_type == DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH;
      const int next_autosmooth = n   &&  n->header.node_type == DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH;
      const int firstseg        = !p  ||  d->header.type != DT_LIQUIFY_PATH_CURVE_TO_V1;
      const int lastseg         = !nn ||  nn->header.type != DT_LIQUIFY_PATH_CURVE_TO_V1;
      const int lineseg         = n   &&  n->header.type == DT_LIQUIFY_PATH_LINE_TO_V1;

      // Program the linear system with equations:
      //
      // 1: straight start  smooth end
      // 2: smooth start    smooth end
      // 3: smooth start    straight end
      // 4: keep start      smooth end
      // 5: keep start      keep end
      // 6: smooth start    keep end
      // 7: keep start      straight end
      // 8: straight start  straight end   (== line)
      // 9: straight start  keep end

      if (lineseg)
      {
        eqn[k] = 5;
        goto done;
      }
      if (!autosmooth && !next_autosmooth)
      {
        eqn[k] = 5;
        goto done;
      }

      if (firstseg && lastseg && !autosmooth && next_autosmooth)
      {
        eqn[k] = 7;
        goto done;
      }
      if (firstseg && lastseg && autosmooth && next_autosmooth)
      {
        eqn[k] = 8;
        goto done;
      }
      if (firstseg && lastseg && autosmooth && !next_autosmooth)
      {
        eqn[k] = 9;
        goto done;
      }

      if (firstseg && autosmooth)
      {
        eqn[k] = 1;
        goto done;
      }
      if (lastseg && autosmooth && next_autosmooth)
      {
        eqn[k] = 3;
        goto done;
      }
      if (lastseg && !autosmooth && next_autosmooth)
      {
        eqn[k] = 7;
        goto done;
      }
      if (autosmooth && !next_autosmooth)
      {
        eqn[k] = 6;
        goto done;
      }
      if (!autosmooth && next_autosmooth)
      {
        eqn[k] = 4;
        goto done;
      }
      eqn[k] = 2; // default

    done:
      ++k;
    }

    smooth_path_linsys (n, pt, c1, c2, eqn);

    // write calculated control points back to list structure
    k = 0;
    for (GList *j = ((GList *)i->data)->next; j != NULL; j = j->next)
    {
      dt_liquify_curve_to_v1_t *d  = (dt_liquify_curve_to_v1_t *) j->data;
      if (d->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
      {
        d->ctrl1 = c1[k];
        d->ctrl2 = c2[k];
      }
      ++k;
    }

    free (pt);
    free (c1);
    free (c2);
    free (eqn);
  }
}

static GList *find_link (GList *paths, dt_liquify_path_data_t *node)
{
  for (GList *i = paths; i != NULL; i = i->next)
    for (GList *j = i->data; j != NULL; j = j->next)
      if (j->data == node)
        return j;
  return NULL;
}

static dt_liquify_path_data_t *find_hovered (GList *paths)
{
  for (GList *i = paths; i != NULL; i = i->next)
    for (GList *j = i->data; j != NULL; j = j->next) {
      dt_liquify_path_data_t *elem = (dt_liquify_path_data_t *) j->data;
      if (elem->header.hovered)
        return elem;
    }
  return NULL;
}

static GList *add_node (GList *paths, dt_liquify_path_data_t *node)
{
  GList *list = g_list_last (paths); assert (list);
  list = g_list_append (list->data, node);

  return paths;
}

static GList *delete_node (GList *paths, dt_liquify_path_data_t *node)
{
  if (!node)
    return paths;

  assert (node->header.type <= DT_LIQUIFY_PATH_CURVE_TO_V1);
  assert (node->header.size <= sizeof (dt_liquify_curve_to_v1_t));

  GList *link = find_link (paths, node); assert (link);
  GList *list = g_list_first (link);
  paths = g_list_remove (paths, list);

  if (link->next && !link->prev)
  {
    // first node
    GList *n = link->next; // g_list_remove_link zeroes these
    // current link becomes next one, copy point and radius/strength of the warp
    *(_get_point (link)) = *(_get_point (link->next));
    ((dt_liquify_path_data_t *) link->data)->warp.radius = ((dt_liquify_path_data_t *) n->data)->warp.radius;
    ((dt_liquify_path_data_t *) link->data)->warp.strength = ((dt_liquify_path_data_t *) n->data)->warp.strength;
    list = g_list_remove_link (list, link->next);
    ((dt_liquify_path_data_t *) n->data)->header.type = 99999; // invalidate
    g_list_free_full (n, free);
  }
  else
  {
    // not first node
    list = g_list_remove_link (list, link);
    node->header.type = 99999; // invalidate
    g_list_free_full (link, free);
  }

  if (list)
    paths = g_list_append (paths, list);

  return paths;
}

static void *palloc (size_t size)
{
  dt_liquify_path_data_t *p = calloc (1, size);
  p->header.size = size;
  p->header.selected = 0;
  p->header.hovered = 0;
  return p;
}

static void init_warp (dt_liquify_warp_t *warp, dt_liquify_warp_type_enum_t type, double complex point)
{
  warp->type     = type;
  warp->point    = point;
  warp->radius   = point;
  warp->strength = point;
  warp->control1 = 0.5;
  warp->control2 = 0.75;
}

static gpointer alloc_move_to (double complex start_point)
{
  dt_liquify_move_to_v1_t* m = palloc (sizeof (dt_liquify_move_to_v1_t));
  m->header.type = DT_LIQUIFY_PATH_MOVE_TO_V1;
  m->header.node_type = DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH;
  m->warp.point = start_point;
  init_warp (&m->warp, DT_LIQUIFY_WARP_TYPE_LINEAR, start_point);
  return m;
}

static gpointer alloc_line_to (double complex end_point)
{
  dt_liquify_line_to_v1_t* l = palloc (sizeof (dt_liquify_line_to_v1_t));
  l->header.type = DT_LIQUIFY_PATH_LINE_TO_V1;
  l->header.node_type = DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH;
  l->warp.point = end_point;
  init_warp (&l->warp, DT_LIQUIFY_WARP_TYPE_LINEAR, end_point);
  return l;
}

static gpointer alloc_curve_to (double complex end_point)
{
  dt_liquify_curve_to_v1_t* c = palloc (sizeof (dt_liquify_curve_to_v1_t));
  c->header.type = DT_LIQUIFY_PATH_CURVE_TO_V1;
  c->header.node_type = DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH;
  c->ctrl1 = c->ctrl2 = c->warp.point = end_point;
  init_warp (&c->warp, DT_LIQUIFY_WARP_TYPE_LINEAR, end_point);
  return c;
}

static void start_drag (dt_iop_liquify_gui_data_t *g, dt_liquify_layer_enum_t layer, dt_liquify_path_data_t *elem)
{
  dt_liquify_hit_t *hit = malloc (sizeof (dt_liquify_hit_t));
  hit->layer = layer;
  hit->elem = elem;
  g->dragging = g_list_append (g->dragging, hit);
}

static void end_drag (dt_iop_liquify_gui_data_t *g)
{
  if (g->dragging)
  {
    PRINT ("End dragging something.\n");
    g_list_free_full (g->dragging, free);
    g->dragging = NULL;
  }
}

static void unselect_all (GList *paths)
{
  for (GList *i = paths; i != NULL; i = i->next)
    for (GList *j = i->data; j != NULL; j = j->next)
      ((dt_liquify_path_data_t *) j->data)->header.selected = 0;
}

static float get_zoom_scale (dt_develop_t *develop)
{
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom ();
  const int closeup = dt_control_get_dev_closeup ();
  return dt_dev_get_zoom_scale (develop, zoom, closeup ? 2 : 1, 1);
}

void gui_post_expose (struct dt_iop_module_t *module,
                      cairo_t *cr,
                      int32_t width,
                      int32_t height,
                      int32_t pointerx,
                      int32_t pointery)
{
  PRINT_FUNC ();

  dt_develop_t *develop = module->dev;
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;
  if (!g)
    return;

  const float bb_width = develop->preview_pipe->backbuf_width;
  const float bb_height = develop->preview_pipe->backbuf_height;
  if (bb_width < 1.0 || bb_height < 1.0)
    return;

  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe (develop, develop->preview_pipe, module);
  if (!piece)
    return;

  update_warp_count (g);
  smooth_paths_linsys (g);

  // You're not supposed to understand this
  const float zoom_x = dt_control_get_dev_zoom_x ();
  const float zoom_y = dt_control_get_dev_zoom_y ();
  const float zoom_scale = get_zoom_scale (develop);
  cairo_translate (cr, 0.5 * width, 0.5 * height); // origin @ center of view
  cairo_scale     (cr, zoom_scale, zoom_scale);    // the zoom
  cairo_translate (cr, -bb_width * (0.5 + zoom_x), -bb_height * (0.5 + zoom_y));

  // setup CAIRO coordinate system
  const float scale = MAX (bb_width, bb_height);
  cairo_scale (cr, scale, scale);

  dt_pthread_mutex_lock (&g->lock);
  GList *paths = copy_paths (g->paths);
  dt_pthread_mutex_unlock (&g->lock);

  // distort all points in one go.  distorting is an expensive
  // operation because it locks the whole pipe.
  distort_paths_raw_to_cairo (module, piece, paths);

  draw_paths (module, cr, 1.0 / (scale * zoom_scale), paths);
  PRINT ("widget = %d %d, scale = %f, zoom_scale = %f\n", width, height, scale, zoom_scale);

  free_paths (paths);
}

void gui_focus (struct dt_iop_module_t *module, gboolean in)
{
  PRINT_FUNC ();

  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;
  g->mouse_pointer_in_widget = module->enabled && in;
}

static void sync_pipe (struct dt_iop_module_t *module, bool history)
{
  PRINT_FUNC_ARGS ("%d", history);

  const dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;

  if (history)
  {
    // something definitive has happened like button release ... so
    // redraw pipe
    serialize_params (module, g->paths);
    dt_dev_add_history_item (darktable.develop, module, TRUE);
  }
  else
  {
    // only moving mouse around, pointing at things or dragging ... so
    // give some cairo feedback, but don't redraw pipe
    dt_control_queue_redraw_center ();
  }
}

/**
 * \defgroup User Actions
 *
 * right-click on node:       Delete node.
 * right-click on path:       Delete whole path.
 *
 * ctrl+click on node:        Cycle symmetrical, smooth, cusp, autosmooth
 * ctrl+click on path:        Add node
 * ctrl+alt+click on path:    Change line / bezier
 *
 * ctrl+click on strength:    Cycle linear, grow, shrink
 *
 * @{
 */

int mouse_moved (struct dt_iop_module_t *module,
                 double x,
                 double y,
                 double pressure,
                 int which)
{
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;
  int handled = 0;

  dt_develop_t *develop = module->dev;
  const dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe (develop, develop->preview_pipe, module);
  if (!piece)
    return 0;

  const double complex pt_cairo = transform_view_to_cairo (module, piece, x, y);
  const double complex pt = distort_point_cairo_to_raw (module, piece, pt_cairo);
  const double zoom_scale = get_zoom_scale (develop);
  const double scale = UI_SCALE;

  dt_pthread_mutex_lock (&g->lock);

  g->last_mouse_pos = pt;
  const int drag_p = detect_drag (g, scale, pt);

  // Don't hit test while dragging, you'd only hit the dragged thing
  // anyway.
  if (!g->dragging)
  {

    dt_liquify_hit_t hit = hit_test_paths (module, scale, g->fake_cr, g->paths, pt);
    dt_liquify_path_data_t *last_hovered = find_hovered (g->paths);
    if (hit.elem != last_hovered ||
        (last_hovered && hit.elem && hit.elem->header.hovered != last_hovered->header.hovered))
    {
      if (hit.elem)
        hit.elem->header.hovered = hit.layer;
      if (last_hovered)
        last_hovered->header.hovered = 0;
      // change in hover display
      hint (dt_liquify_layers[hit.layer].hint);
      handled = 1;
      goto done;
    }
  }

  if (drag_p && !g->dragging && g->last_hit.elem)
  {
    // start dragging
    // PRINT ("Start dragging: %f %f\n", x, y);
    start_drag (g, g->last_hit.layer, g->last_hit.elem);
  }

  if (g->dragging)
  {
    // PRINT ("Dragging: %f %f\n", x, y);

    for (GList *i = g->dragging; i != NULL; i = i->next)
    {
      const dt_liquify_hit_t *hit = i->data; assert (hit && hit->elem);
      dt_liquify_path_data_t *d = hit->elem;
      const GList *link = find_link (g->paths, d); assert (link);
      dt_liquify_path_data_t *n = link->next ? link->next->data : NULL;
      dt_liquify_path_data_t *p = link->prev ? link->prev->data : NULL;

      const double complex *start_pt = _get_point (link);

      switch (hit->layer)
      {
      case DT_LIQUIFY_LAYER_CENTERPOINT:
        switch (d->header.type)
        {
        case DT_LIQUIFY_PATH_CURVE_TO_V1:
          d->curve_to_v1.ctrl2 += pt - d->point;
          // fall thru
        case DT_LIQUIFY_PATH_MOVE_TO_V1:
        case DT_LIQUIFY_PATH_LINE_TO_V1:
          if (n && n->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
            n->curve_to_v1.ctrl1 += pt - d->point;
          d->warp.radius   += pt - d->point;
          d->warp.strength += pt - d->point;
          d->point = pt;
          break;
        default:
          break;
        }
        break;

      case DT_LIQUIFY_LAYER_CTRLPOINT1:
        switch (d->header.type)
        {
        case DT_LIQUIFY_PATH_CURVE_TO_V1:
          d->curve_to_v1.ctrl1 = pt;
          if (p && p->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
          {
            switch (p->header.node_type)
            {
            case DT_LIQUIFY_NODE_TYPE_SMOOTH:
              p->curve_to_v1.ctrl2 = p->point +
                cabs (p->point - p->curve_to_v1.ctrl2) *
                cexp (carg (p->point - pt) * I);
              break;
            case DT_LIQUIFY_NODE_TYPE_SYMMETRICAL:
              p->curve_to_v1.ctrl2 = 2 * p->point - pt;
              break;
            default:
              break;
            }
          }
          break;
        default:
          break;
        }
        break;

      case DT_LIQUIFY_LAYER_CTRLPOINT2:
        switch (d->header.type)
        {
        case DT_LIQUIFY_PATH_CURVE_TO_V1:
          d->curve_to_v1.ctrl2 = pt;
          if (n && n->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
          {
            switch (d->header.node_type)
            {
            case DT_LIQUIFY_NODE_TYPE_SMOOTH:
              n->curve_to_v1.ctrl1 = d->point +
                cabs (d->point - n->curve_to_v1.ctrl1) *
                cexp (carg (d->point - pt) * I);
              break;
            case DT_LIQUIFY_NODE_TYPE_SYMMETRICAL:
              n->curve_to_v1.ctrl1 = 2 * d->point - pt;
              break;
            default:
              break;
            }
          }
          break;
        default:
          break;
        }
        break;

      case DT_LIQUIFY_LAYER_RADIUSPOINT:
        d->warp.radius = pt;
        dt_conf_set_float(CONF_RADIUS, cabs(d->warp.radius - d->point));
        break;

      case DT_LIQUIFY_LAYER_STRENGTHPOINT:
        d->warp.strength = pt;
        break;

      case DT_LIQUIFY_LAYER_HARDNESSPOINT1:
        d->warp.control1 = MIN (1.0, cabs (pt - *start_pt) / cabs (d->warp.radius - *start_pt));
        break;

      case DT_LIQUIFY_LAYER_HARDNESSPOINT2:
        d->warp.control2 = MIN (1.0, cabs (pt - *start_pt) / cabs (d->warp.radius - *start_pt));
        break;

      default:
        break;
      }
    }
    handled = 1;
  }
  handled = g->last_hit.elem != NULL;

done:
  dt_pthread_mutex_unlock (&g->lock);
  if (handled) {
    sync_pipe (module, handled == 2);
  }
  return handled;
}

int button_pressed (struct dt_iop_module_t *module,
                    double x,
                    double y,
                    double pressure,
                    int which,
                    int type,
                    uint32_t state)
{
  PRINT_FUNC_ARGS ("%lf %lf", x, y);

  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;
  int handled = 0;

  dt_develop_t *develop = module->dev;
  const dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe (develop, develop->preview_pipe, module);
  if (!piece)
    return 0;

  const double complex pt_cairo = transform_view_to_cairo (module, piece, x, y);
  const double complex pt = distort_point_cairo_to_raw (module, piece, pt_cairo);
  const double zoom_scale = get_zoom_scale (develop);
  const double scale = UI_SCALE;

  dt_pthread_mutex_lock (&g->lock);

  g->last_mouse_pos = pt;
  g->last_mouse_mods = state & gtk_accelerator_get_default_mod_mask ();
  if (which == 1)
    g->last_button1_pressed_pos = pt;

  if (!g->dragging)
    // while dragging you would always hit the dragged thing
    g->last_hit = hit_test_paths (module, scale, g->fake_cr, g->paths, pt);

  PRINT ("Hit testing pt: %f %f\n", creal (pt), cimag (pt));
  PRINT ("Hit on %d\n", g->last_hit.layer);

  if (which == 2) goto done;

  // Point tool

  if (which == 1 && gtk_toggle_button_get_active (g->btn_point_tool))
  {
    // always end dragging before manipulating the path list to avoid
    // dangling pointers
    end_drag (g);

    // start a new path
    PRINT ("New point: %lf %lf\n", x, y);
    g->temp = alloc_move_to (pt);
    g->temp->warp.radius   = pt +
      (dt_conf_key_exists(CONF_RADIUS) ? dt_conf_get_float(CONF_RADIUS) : GET_UI_WIDTH (DEFAULT_RADIUS));
    g->temp->warp.strength = pt + GET_UI_WIDTH (DEFAULT_STRENGTH);
    g->status |= DT_LIQUIFY_STATUS_NEW;
    g->paths = g_list_append (g->paths, g_list_append (NULL, g->temp));

    start_drag (g, DT_LIQUIFY_LAYER_STRENGTHPOINT, g->temp);
    g->last_hit = NOWHERE;
    handled = 1;
    goto done;
  }

  // Line tool or curve tool

  if (which == 1 && (gtk_toggle_button_get_active (g->btn_line_tool)
                     || gtk_toggle_button_get_active (g->btn_curve_tool)))
  {
    // always end dragging before manipulating the path list to avoid
    // dangling pointers
    end_drag (g);
    if (!g->temp)
    {
      if (g->last_hit.layer == DT_LIQUIFY_LAYER_CENTERPOINT)
      {
        // continue path
        PRINT ("Continuing path: %lf %lf\n", x, y);
        g->temp = g->last_hit.elem;
      }
      else
      {
        // start a new path
        PRINT ("New path: %lf %lf\n", x, y);
        g->temp = alloc_move_to (pt);
        g->temp->warp.radius   = pt +
          (dt_conf_key_exists(CONF_RADIUS) ? dt_conf_get_float(CONF_RADIUS) : GET_UI_WIDTH (DEFAULT_RADIUS));
        g->temp->warp.strength = pt + GET_UI_WIDTH (DEFAULT_STRENGTH);
        g->paths = g_list_append (g->paths, g_list_append (NULL, g->temp)); // owns node now
      }
    }
    g->last_hit = NOWHERE;
    if (gtk_toggle_button_get_active (g->btn_curve_tool))
    {
      start_drag (g, DT_LIQUIFY_LAYER_CTRLPOINT1, g->temp);
    }
    g->status |= DT_LIQUIFY_STATUS_NEW;
    handled = 1;
    goto done;
  }

  // Node tool

  if (gtk_toggle_button_get_active (g->btn_node_tool))
  {
    if (which == 1 && (g->last_mouse_mods == GDK_CONTROL_MASK) &&
        (g->last_hit.layer == DT_LIQUIFY_LAYER_CENTERPOINT))
    {
      // cycle node type: smooth -> cusp etc.
      dt_liquify_path_data_t *node = g->last_hit.elem;
      node->header.node_type = (node->header.node_type + 1) % DT_LIQUIFY_NODE_TYPE_LAST;
      handled = 1;
      goto done;
    }
    if (which == 1 && (g->last_mouse_mods == GDK_CONTROL_MASK) &&
        (g->last_hit.layer == DT_LIQUIFY_LAYER_STRENGTHPOINT))
    {
      // cycle warp type: linear -> radial etc.
      if (g->last_hit.elem->header.type == DT_LIQUIFY_PATH_MOVE_TO_V1)
      {
        dt_liquify_warp_t *warp = &g->last_hit.elem->warp;
        warp->type = (warp->type + 1) % DT_LIQUIFY_WARP_TYPE_LAST;
      }
      handled = 1;
      goto done;
    }
  }

done:
  dt_pthread_mutex_unlock (&g->lock);
  if (handled)
    sync_pipe (module, true);
  return handled;
}

int button_released (struct dt_iop_module_t *module,
                     double x,
                     double y,
                     int which,
                     uint32_t state)
{
  PRINT_FUNC_ARGS ("%lf %lf", x, y);

  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;
  int handled = 0;

  dt_develop_t *develop = module->dev;
  const dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe (develop, develop->preview_pipe, module);
  if (!piece)
    return 0;

  const double complex pt_cairo = transform_view_to_cairo (module, piece, x, y);
  const double complex pt = distort_point_cairo_to_raw (module, piece, pt_cairo);
  const double zoom_scale = get_zoom_scale (develop);
  const double scale = UI_SCALE;

  dt_pthread_mutex_lock (&g->lock);

  g->last_mouse_pos = pt;

  const bool dragged = detect_drag (g, scale, pt);

  if (which == 1 && g->temp && (g->status & DT_LIQUIFY_STATUS_NEW))
  {
    end_drag (g);
    // double zoom_scale = get_zoom_scale (develop);
    if (gtk_toggle_button_get_active (g->btn_point_tool))
    {
      PRINT ("Add point at: %f %f\n", x, y);
      // user released without dragging, set a default strength vector
      if (!dragged)
      {
        PRINT ("No drag detected. Setting point strength to default.\n");
        g->temp->warp.strength = pt + GET_UI_WIDTH (DEFAULT_STRENGTH);
      }
      g->temp = NULL; // a point is done
      gtk_toggle_button_set_active (g->btn_node_tool, 1);
    }
    if (gtk_toggle_button_get_active (g->btn_line_tool))
    {
      PRINT ("Add line to: %f %f\n", x, y);
      g->temp = alloc_line_to (pt);
      g->temp->warp.radius   = pt +
        (dt_conf_key_exists(CONF_RADIUS) ? dt_conf_get_float(CONF_RADIUS) : GET_UI_WIDTH (DEFAULT_RADIUS));
      g->temp->warp.strength = pt + GET_UI_WIDTH (DEFAULT_STRENGTH);
      g->paths = add_node (g->paths, g->temp);
      start_drag (g, DT_LIQUIFY_LAYER_CENTERPOINT, g->temp);
    }
    if (gtk_toggle_button_get_active (g->btn_curve_tool))
    {
      // user dragged, make it a symmetrical node
      if (dragged)
      {
        PRINT ("Drag detected. Setting node to symmetrical.\n");
        g->temp->header.node_type = DT_LIQUIFY_NODE_TYPE_SYMMETRICAL;
      }
      PRINT ("Add curve to: %f %f\n", x, y);
      g->temp = alloc_curve_to (pt);
      g->temp->warp.radius = pt +
        (dt_conf_key_exists(CONF_RADIUS) ? dt_conf_get_float(CONF_RADIUS) : GET_UI_WIDTH (DEFAULT_RADIUS));
      g->temp->warp.strength = pt + GET_UI_WIDTH (DEFAULT_STRENGTH);
      g->paths = add_node (g->paths, g->temp);
      start_drag (g, DT_LIQUIFY_LAYER_CENTERPOINT, g->temp);
    }
    g->status &= ~DT_LIQUIFY_STATUS_NEW;
    handled = 1;
    goto done;
  }

  if (which == 1 && g->dragging)
  {
    end_drag (g);
    handled = 2;
    goto done;
  }

  // right click == cancel or delete
  if (which == 3)
  {
    end_drag (g);

    // cancel line or curve creation
    if (g->temp)
    {
      g->paths = delete_node (g->paths, g->temp);
      g->temp = NULL;
      gtk_toggle_button_set_active (g->btn_node_tool, 1);
      handled = 2;
      goto done;
    }

    // right click on background toggles node tool
    if (g->last_hit.layer == DT_LIQUIFY_LAYER_BACKGROUND)
    {
      gtk_toggle_button_set_active (g->btn_node_tool,
                                    !gtk_toggle_button_get_active (g->btn_node_tool));
      handled = 1;
      goto done;
    }

    // delete node
    if (g->last_hit.layer == DT_LIQUIFY_LAYER_CENTERPOINT)
    {
      g->paths = delete_node (g->paths, g->last_hit.elem);
      g->last_hit = NOWHERE;
      handled = 2;
      goto done;
    }
    // delete shape
    if (g->last_hit.layer == DT_LIQUIFY_LAYER_PATH)
    {
      GList *l = g_list_first (find_link (g->paths, g->last_hit.elem));
      g->paths = g_list_remove (g->paths, l);
      g_list_free_full (l, free);
      g->last_hit = NOWHERE;
      handled = 2;
      goto done;
    }
    goto done;
  }

  // Node tool

  if (gtk_toggle_button_get_active (g->btn_node_tool))
  {
    if (which == 1 && g->last_mouse_mods == 0 && !detect_drag (g, scale, pt))
    {
      // select/unselect start/endpoint and clear previous selections
      if (g->last_hit.layer == DT_LIQUIFY_LAYER_CENTERPOINT)
      {
        PRINT ("Selected: %f %f\n", x, y);
        const int oldsel = !!g->last_hit.elem->header.selected;
	unselect_all (g->paths);
        g->last_hit.elem->header.selected = oldsel ? 0 : g->last_hit.layer;
        handled = 1;
        goto done;
      }
      // unselect all
      if (g->last_hit.layer == DT_LIQUIFY_LAYER_BACKGROUND)
      {
        PRINT ("Unselect all: %f %f\n", x, y);
        unselect_all (g->paths);
        handled = 1;
        goto done;
      }
    }
    if (which == 1 && g->last_mouse_mods == GDK_SHIFT_MASK && !detect_drag (g, scale, pt))
    {
      // select/unselect start/endpoint and keep previous selections
      if (g->last_hit.layer == DT_LIQUIFY_LAYER_CENTERPOINT)
      {
        PRINT ("Selected: %f %f\n", x, y);
        const int oldsel = !!g->last_hit.elem->header.selected;
        g->last_hit.elem->header.selected = oldsel ? 0 : g->last_hit.layer;
        handled = 1;
        goto done;
      }
    }
    if (which == 1 && (g->last_mouse_mods == GDK_CONTROL_MASK) && !detect_drag (g, scale, pt))
    {
      // add node
      if (g->last_hit.layer == DT_LIQUIFY_LAYER_PATH)
      {
        dt_liquify_path_data_t *e = g->last_hit.elem;
        GList *link = find_link (g->paths, e);
        if (link && link->prev && e->header.type  == DT_LIQUIFY_PATH_CURVE_TO_V1)
        {
	  // add node to curve
          dt_liquify_curve_to_v1_t *c = (dt_liquify_curve_to_v1_t *) g->last_hit.elem;
          double complex p0 = *_get_point (link->prev);
          double complex p1 = c->ctrl1;
          double complex p2 = c->ctrl2;
          double complex p3 = e->point;
          const double t = find_nearest_on_curve_t (p0, p1, p2, p3, pt, INTERPOLATION_POINTS);
          casteljau (&p0, &c->ctrl1, &c->ctrl2, &e->point, t);
          casteljau (&p3, &p2, &p1, &p0, 1.0 - t);
          dt_liquify_curve_to_v1_t *tmp = alloc_curve_to (p3);
          tmp->warp = e->warp;
          tmp->ctrl1 = p1;
          tmp->ctrl2 = p2;
          link = g_list_insert_before (g_list_first (link), link->next, tmp);

          handled = 2;
          goto done;
        }
        if (link && link->prev && e->header.type  == DT_LIQUIFY_PATH_LINE_TO_V1)
        {
	  // add node to line
          const double complex p0 = *_get_point (link->prev);
          const double complex p1 = e->point;
          const double t = find_nearest_on_line_t (p0, p1, pt);
          dt_liquify_line_to_v1_t *tmp = alloc_line_to (p1);
          tmp->warp = e->warp;
          e->point = p0 + t * (p1 - p0);
          link = g_list_insert_before (g_list_first (link), link->next, tmp);

          handled = 2;
          goto done;
        }
      }
    }
    if (which == 1
        && (g->last_mouse_mods == (GDK_MOD1_MASK | GDK_CONTROL_MASK))
        && !detect_drag (g, scale, pt))
    {
      if (g->last_hit.layer == DT_LIQUIFY_LAYER_PATH)
      {
        // change segment
        dt_liquify_path_data_t *e = g->last_hit.elem;
        GList *link = find_link (g->paths, e);
        if (link && link->prev && e->header.type  == DT_LIQUIFY_PATH_CURVE_TO_V1)
        {
	  PRINT ("Change curve to line.\n");
          // curve -> line
          const dt_liquify_curve_to_v1_t *c = (dt_liquify_curve_to_v1_t *) e;
          dt_liquify_line_to_v1_t *tmp = alloc_line_to (e->point);
          tmp->warp = c->warp;
          link = g_list_insert_before (g_list_first (link), link->next, tmp);
          delete_node (g->paths, e);

          handled = 2;
          goto done;
        }
        if (link && link->prev && e->header.type  == DT_LIQUIFY_PATH_LINE_TO_V1)
        {
	  PRINT ("Change line to curve.\n");
          // line -> curve
          const double complex p0 = *_get_point (link->prev);
          const double complex p1 = e->point;
          dt_liquify_curve_to_v1_t *tmp = alloc_curve_to (p1);
          tmp->ctrl1 = (2 * p0 +     p1) / 3.0;
          tmp->ctrl2 = (    p0 + 2 * p1) / 3.0;
          tmp->warp = e->warp;
          link = g_list_insert_before (g_list_first (link), link->next, tmp);
          delete_node (g->paths, e);

          handled = 2;
          goto done;
        }
      }
    }
  }

done:
  if (which == 1)
    g->last_button1_pressed_pos = -1;
  g->last_hit = NOWHERE;
  dt_pthread_mutex_unlock (&g->lock);
  if (handled) {
    update_warp_count (g);
    sync_pipe (module, handled == 2);
  }
  return handled;
}

/**@}*/

static void _liquify_cairo_paint_no_tool
(cairo_t *cr, const gint x, const gint y, const gint w, const gint h, const gint flags);

static void _liquify_cairo_paint_point_tool
(cairo_t *cr, const gint x, const gint y, const gint w, const gint h, const gint flags);

static void _liquify_cairo_paint_line_tool
(cairo_t *cr, const gint x, const gint y, const gint w, const gint h, const gint flags);

static void _liquify_cairo_paint_curve_tool
(cairo_t *cr, const gint x, const gint y, const gint w, const gint h, const gint flags);

static void _liquify_cairo_paint_node_tool
(cairo_t *cr, const gint x, const gint y, const gint w, const gint h, const gint flags);

/* We need this only because darktable has no radiobutton support. */
static void btn_make_radio_callback (GtkToggleButton *btn, dt_iop_module_t *module)
{
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;
  hint ("");
  if (gtk_toggle_button_get_active (btn))
  {
    gtk_toggle_button_set_active (g->btn_point_tool, btn == g->btn_point_tool);
    gtk_toggle_button_set_active (g->btn_line_tool,  btn == g->btn_line_tool);
    gtk_toggle_button_set_active (g->btn_curve_tool, btn == g->btn_curve_tool);
    gtk_toggle_button_set_active (g->btn_node_tool,  btn == g->btn_node_tool);
    if (btn == g->btn_point_tool)
      hint (_("click and drag to add point"));
    if (btn == g->btn_line_tool)
      hint (_("click to add line"));
    if (btn == g->btn_curve_tool)
      hint (_("click to add curve"));
    if (btn == g->btn_node_tool)
      hint (_("click to edit nodes"));
  }
  gtk_toggle_button_set_active (g->btn_no_tool, 0);
  sync_pipe (module, false);
}

void gui_update (dt_iop_module_t *module)
{
  PRINT_FUNC ();

  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;
  g->paths = unserialize_params (module->params);
  debug_params ("unserialized in gui_update ()", module->params);
}

void gui_init (dt_iop_module_t *module)
{
  PRINT_FUNC ();

  const int bs = DT_PIXEL_APPLY_DPI(14);

  module->gui_data = malloc (sizeof (dt_iop_liquify_gui_data_t));
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;
  // dt_iop_liquify_params_t *p = (dt_iop_liquify_params_t *) module->params;

  // A dummy surface for calculations only, no drawing.
  cairo_surface_t *cs = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 1, 1);
  g->fake_cr = cairo_create (cs);
  cairo_surface_destroy (cs);

  g->dragging = NULL;
  g->temp = NULL;
  g->status = 0;
  g->last_mouse_pos =
  g->last_button1_pressed_pos = -1;
  g->last_hit = NOWHERE;
  g->mouse_pointer_in_widget = 0;
  dt_pthread_mutex_init (&g->lock, NULL);

  // connect to history change signal for invalidating distort_transform data
  // dt_control_signal_connect (darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
  // G_CALLBACK (dt_liquify_history_change_callback), module);

  module->widget = gtk_box_new (GTK_ORIENTATION_VERTICAL, 5);

  GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 5);
  g_object_set (G_OBJECT (hbox), "tooltip-text",
                _("use a tool to add warps.\nright-click to remove a warp."), (char *) NULL);

  gtk_box_pack_start ((GtkBox *) hbox, gtk_label_new (_("number of warps:")), FALSE, TRUE, 0);
  g->label = (GtkLabel *) gtk_label_new ("-");
  gtk_box_pack_start ((GtkBox *) hbox, (GtkWidget *) g->label, FALSE, TRUE, 0);

  g->btn_node_tool = (GtkToggleButton *) dtgtk_togglebutton_new (
    _liquify_cairo_paint_node_tool, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  g_signal_connect (G_OBJECT (g->btn_node_tool), "toggled", G_CALLBACK (btn_make_radio_callback), module);
  g_object_set (G_OBJECT (g->btn_node_tool), "tooltip-text", _("node tool: edit, add and delete nodes"), (char *) NULL);
  gtk_toggle_button_set_active (g->btn_node_tool, 0);
  gtk_widget_set_size_request ((GtkWidget *) g->btn_node_tool, bs, bs);
  gtk_box_pack_end ((GtkBox *) (hbox), (GtkWidget *) g->btn_node_tool, FALSE, FALSE, 0);

  g->btn_curve_tool = (GtkToggleButton *) dtgtk_togglebutton_new (
    _liquify_cairo_paint_curve_tool, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  g_signal_connect (G_OBJECT (g->btn_curve_tool), "toggled", G_CALLBACK (btn_make_radio_callback), module);
  g_object_set (G_OBJECT (g->btn_curve_tool), "tooltip-text", _("curve tool: draw curves"), (char *) NULL);
  gtk_toggle_button_set_active (g->btn_curve_tool, 0);
  gtk_widget_set_size_request ((GtkWidget *) g->btn_curve_tool, bs, bs);
  gtk_box_pack_end ((GtkBox *) hbox, (GtkWidget *) g->btn_curve_tool, FALSE, FALSE, 0);

  g->btn_line_tool = (GtkToggleButton *) dtgtk_togglebutton_new (
    _liquify_cairo_paint_line_tool, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  g_signal_connect (G_OBJECT (g->btn_line_tool), "toggled", G_CALLBACK (btn_make_radio_callback), module);
  g_object_set (G_OBJECT (g->btn_line_tool), "tooltip-text", _("line tool: draw lines"), (char *) NULL);
  gtk_toggle_button_set_active (g->btn_line_tool, 0);
  gtk_widget_set_size_request ((GtkWidget *) g->btn_line_tool, bs, bs);
  gtk_box_pack_end ((GtkBox *) hbox, (GtkWidget *) g->btn_line_tool, FALSE, FALSE, 0);

  g->btn_point_tool = (GtkToggleButton *) dtgtk_togglebutton_new (
    _liquify_cairo_paint_point_tool, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  g_signal_connect (G_OBJECT (g->btn_point_tool), "toggled", G_CALLBACK (btn_make_radio_callback), module);
  g_object_set (G_OBJECT (g->btn_point_tool), "tooltip-text", _("point tool: draw points"), (char *) NULL);
  gtk_toggle_button_set_active (g->btn_point_tool, 0);
  gtk_widget_set_size_request ((GtkWidget *) g->btn_point_tool, bs, bs);
  gtk_box_pack_end ((GtkBox *) hbox, (GtkWidget *) g->btn_point_tool, FALSE, FALSE, 0);

  g->btn_no_tool = (GtkToggleButton *) dtgtk_togglebutton_new (
    _liquify_cairo_paint_no_tool, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  g_signal_connect (G_OBJECT (g->btn_no_tool), "toggled", G_CALLBACK (btn_make_radio_callback), module);
  g_object_set (G_OBJECT (g->btn_no_tool), "tooltip-text", _("disable all tools"), (char *) NULL);
  gtk_toggle_button_set_active (g->btn_no_tool, 0);
  gtk_widget_set_size_request ((GtkWidget *) g->btn_no_tool, bs, bs);
  gtk_box_pack_end ((GtkBox *) hbox, (GtkWidget *) g->btn_no_tool, FALSE, FALSE, 0);

  gtk_box_pack_start ((GtkBox *) module->widget, hbox, TRUE, TRUE, 0);

  // enable advanced input devices to get pressure readings and stuff like that
  // dt_gui_enable_extended_input_devices ();

  dt_liquify_layers[DT_LIQUIFY_LAYER_PATH].hint           = _("ctrl+click to add node\nright click to remove path");
  dt_liquify_layers[DT_LIQUIFY_LAYER_CENTERPOINT].hint    = _("click and drag to move - click : linear or feathered\nctrl+click : symmetrical, smooth, cusp, autosmooth\nright-click to remove");
  dt_liquify_layers[DT_LIQUIFY_LAYER_CTRLPOINT1].hint     = _("drag to change shape of path");
  dt_liquify_layers[DT_LIQUIFY_LAYER_CTRLPOINT2].hint     = _("drag to change shape of path");
  dt_liquify_layers[DT_LIQUIFY_LAYER_RADIUSPOINT].hint    = _("drag to adjust warp radius");
  dt_liquify_layers[DT_LIQUIFY_LAYER_HARDNESSPOINT1].hint = _("drag to adjust hardness (center)");
  dt_liquify_layers[DT_LIQUIFY_LAYER_HARDNESSPOINT2].hint = _("drag to adjust hardness (feather)");
  dt_liquify_layers[DT_LIQUIFY_LAYER_STRENGTHPOINT].hint  = _("drag to adjust warp strength\nctrl+click : linear, grow, and shrink");
}

void gui_cleanup (dt_iop_module_t *module)
{
  PRINT_FUNC ();

  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;
  if (g)
  {
    cairo_destroy (g->fake_cr);
    free_paths (g->paths);
    dt_pthread_mutex_destroy (&g->lock);
    free (g);
  }
  module->gui_data = NULL;
}

void init_key_accels (dt_iop_module_so_t *module)
{
  dt_accel_register_iop (module, FALSE, NC_("accel", "point tool"),     0, 0);
  dt_accel_register_iop (module, FALSE, NC_("accel", "line tool"),      0, 0);
  dt_accel_register_iop (module, FALSE, NC_("accel", "curve tool"),     0, 0);
  dt_accel_register_iop (module, FALSE, NC_("accel", "node tool"),      0, 0);
  dt_accel_register_iop (module, FALSE, NC_("accel", "disable tools"),  0, 0);
}

void connect_key_accels (dt_iop_module_t *module)
{
  const dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *) module->gui_data;

  dt_accel_connect_button_iop (module, "disable tools", GTK_WIDGET (g->btn_no_tool));
  dt_accel_connect_button_iop (module, "point tool",    GTK_WIDGET (g->btn_point_tool));
  dt_accel_connect_button_iop (module, "line tool",     GTK_WIDGET (g->btn_line_tool));
  dt_accel_connect_button_iop (module, "curve tool",    GTK_WIDGET (g->btn_curve_tool));
  dt_accel_connect_button_iop (module, "node tool",     GTK_WIDGET (g->btn_node_tool));
}

/**
 * \defgroup Button paint functions
 *
 * @{
 */

#define PREAMBLE                                        \
  cairo_save (cr);                                      \
  const gint s = MIN (w, h);                            \
  cairo_translate (cr, x + (w / 2.0) - (s / 2.0),       \
                   y + (h / 2.0) - (s / 2.0));          \
  cairo_scale (cr, s, s);                               \
  cairo_push_group (cr);                                \
  cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 1.0);       \
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);        \
  cairo_set_line_width (cr, 0.2);

#define POSTAMBLE                                               \
  cairo_pop_group_to_source (cr);                               \
  cairo_paint_with_alpha (cr, flags & CPF_ACTIVE ? 1.0 : 0.5);  \
  cairo_restore (cr);

static void _liquify_cairo_paint_no_tool (cairo_t *cr,
                                          const gint x, const gint y, const gint w, const gint h, const gint flags)
{
  PREAMBLE;
  cairo_set_line_width (cr, 0.1);
  cairo_move_to (cr, 0.3, 0.7);
  cairo_line_to (cr, 0.7, 0.3);

  cairo_move_to (cr, 0.3, 0.3);
  cairo_line_to (cr, 0.7, 0.7);
  cairo_stroke (cr);
  POSTAMBLE;
}

static void _liquify_cairo_paint_point_tool (cairo_t *cr,
                                             const gint x, const gint y, const gint w, const gint h, const gint flags)
{
  PREAMBLE;
  cairo_new_sub_path (cr);
  cairo_arc (cr, 0.5, 0.5, 0.2, 0.0, 2 * M_PI);
  cairo_fill (cr);
  POSTAMBLE;
}

static void _liquify_cairo_paint_line_tool (cairo_t *cr,
                                            const gint x, const gint y, const gint w, const gint h, const gint flags)
{
  PREAMBLE;
  cairo_move_to (cr, 0.1, 0.9);
  cairo_line_to (cr, 0.9, 0.1);
  cairo_stroke (cr);
  POSTAMBLE;
}

static void _liquify_cairo_paint_curve_tool (cairo_t *cr,
                                             const gint x, const gint y, const gint w, const gint h, const gint flags)
{
  PREAMBLE;
  cairo_move_to (cr, 0.1, 0.9);
  cairo_curve_to (cr, 0.1, 0.5, 0.5, 0.1, 0.9, 0.1);
  cairo_stroke (cr);
  POSTAMBLE;
}

static void _liquify_cairo_paint_node_tool (cairo_t *cr,
                                            const gint x, const gint y, const gint w, const gint h, const gint flags)
{
  PREAMBLE;
  const double dashed[] = {0.2, 0.2};
  cairo_set_dash (cr, dashed, 2, 0);
  cairo_set_line_width (cr, 0.1);

  cairo_arc (cr, 0.75, 0.75, 0.75, 2.8, 4.7124);
  cairo_stroke (cr);

  cairo_rectangle (cr, 0.2, 0.0, 0.4, 0.4);
  cairo_fill (cr);

  cairo_move_to (cr, 0.4,  0.2);
  cairo_line_to (cr, 0.5,  1.0);
  cairo_line_to (cr, 0.9,  0.7);
  cairo_close_path (cr);
  cairo_fill (cr);
  POSTAMBLE;
}

/** @} */


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
