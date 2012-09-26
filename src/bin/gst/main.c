#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <gst/gst.h>

#include <Eina.h>

#include "shmfile.h"
#include "timeout.h"

#define DATA32  unsigned int

#define GST_DBG

#ifdef GST_DBG
#define D(fmt, args...) fprintf(stderr, fmt, ## args)
#else
#define D(fmt, args...)
#endif

#define CAPS "video/x-raw-rgb,bpp=(int)32,depth=(int)32,endianness=(int)4321,red_mask=(int)0x0000ff00, green_mask=(int)0x00ff0000, blue_mask=(int)0xff000000"

static GstElement *pipeline = NULL;
static GstElement *sink = NULL;
static gint64      duration = -1;

int   width = 0;
int   height = 0;
void *data = NULL;


static Eina_Bool
_gst_init(const char *filename)
{
   GstPad              *pad;
   GstCaps             *caps;
   GstStructure        *structure;
   gchar               *descr;
   gchar               *uri;
   GError              *error = NULL;
   GstFormat            format;
   GstStateChangeReturn ret;

   if (!filename || !*filename)
     return EINA_FALSE;

   if (!gst_init_check(NULL, NULL, &error))
     return EINA_FALSE;

   if ((*filename == '/') || (*filename == '~'))
     {
        uri = g_filename_to_uri(filename, NULL, NULL);
        if (!uri)
          {
             D("could not create new uri from %s", filename);
             goto unref_pipeline;
          }
     }
   else
     uri = strdup(filename);

   D("Setting file %s\n", uri);

   descr = g_strdup_printf("uridecodebin uri=%s ! ffmpegcolorspace ! "
      " appsink name=sink caps=\"" CAPS "\"", uri);
   pipeline = gst_parse_launch(descr, &error);
   free(uri);

   if (error != NULL)
     {
        D("could not construct pipeline: %s\n", error->message);
        g_error_free (error);
        goto gst_shutdown;
     }

   sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");

   ret = gst_element_set_state (pipeline, GST_STATE_PAUSED);
   switch (ret)
     {
     case GST_STATE_CHANGE_FAILURE:
        D("failed to play the file\n");
        goto unref_pipeline;
     case GST_STATE_CHANGE_NO_PREROLL:
        D("live sources not supported yet\n");
        goto unref_pipeline;
     default:
        break;
     }

   ret = gst_element_get_state((pipeline), NULL, NULL, GST_CLOCK_TIME_NONE);
   if (ret == GST_STATE_CHANGE_FAILURE)
     {
	D("could not complete pause\n");
        goto unref_pipeline;
     }

   format = GST_FORMAT_TIME;
   gst_element_query_duration (pipeline, &format, &duration);
   if (duration == -1)
     {
	D("could not retrieve the duration, set it to 1s\n");
        duration = 1 * GST_SECOND;
     }

   pad = gst_element_get_static_pad(sink, "sink");
   if (!pad)
     {
	D("could not retrieve the sink pad\n");
        goto unref_pipeline;
     }

   caps = gst_pad_get_negotiated_caps(pad);
   if (!caps)
     goto unref_pad;

   structure = gst_caps_get_structure(caps, 0);

   if (!gst_structure_get_int(structure, "width", &width))
     goto unref_caps;
   if (!gst_structure_get_int(structure, "height", &height))
     goto unref_caps;

   gst_caps_unref(caps);
   gst_object_unref(pad);

   return EINA_TRUE;

 unref_caps:
   gst_caps_unref(caps);
 unref_pad:
   gst_object_unref(pad);
 unref_pipeline:
   gst_element_set_state (pipeline, GST_STATE_NULL);
   gst_object_unref(pipeline);
 gst_shutdown:
   gst_deinit();

   return EINA_FALSE;
}

static void
_gst_shutdown()
{
   gst_element_set_state (pipeline, GST_STATE_NULL);
   gst_object_unref(pipeline);
   gst_deinit();
}

static void
_gst_load_image(int size_w, int size_h)
{
   GstBuffer *buffer;

   D("load image\n");
   gst_element_seek_simple(pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
                           duration / 2);
   g_signal_emit_by_name(sink, "pull-preroll", &buffer, NULL);
   D("load image : %p %d\n", GST_BUFFER_DATA(buffer), GST_BUFFER_SIZE(buffer));

   shm_alloc(width * height * sizeof(DATA32));
   if (!shm_addr) return;
   data = shm_addr;

   memcpy(data, GST_BUFFER_DATA(buffer), GST_BUFFER_SIZE(buffer));
}

int
main(int argc, char **argv)
{
   char *file;
   int i;
   int size_w = 0, size_h = 0;
   int head_only = 0;
   int page = 0;

   if (argc < 2) return -1;
   // file is ALWAYS first arg, other options come after
   file = argv[1];
   for (i = 2; i < argc; i++)
     {
        if      (!strcmp(argv[i], "-head"))
           // asked to only load header, not body/data
           head_only = 1;
        else if (!strcmp(argv[i], "-key"))
          {
             i++;
             page = atoi(argv[i]);
             i++;
          }
        else if (!strcmp(argv[i], "-opt-scale-down-by"))
          { // not used by ps loader
             i++;
             // int scale_down = atoi(argv[i]);
          }
        else if (!strcmp(argv[i], "-opt-dpi"))
          {
             i++;
          }
        else if (!strcmp(argv[i], "-opt-size"))
          { // not used by ps loader
             i++;
             size_w = atoi(argv[i]);
             i++;
             size_h = atoi(argv[i]);
          }
     }

   timeout_init(10);
   
   D("_gst_init_file\n");

   if (!_gst_init(file))
     return -1;
   D("_gst_init done\n");

   if (!head_only)
     {
        _gst_load_image(size_w, size_h);
     }

   D("size...: %ix%i\n", width, height);
   D("alpha..: 0\n");

   printf("size %i %i\n", width, height);
   printf("alpha 0\n");

   if (!head_only)
     {
        if (shm_fd >= 0)
          {
            printf("shmfile %s\n", shmfile);
          }
        else
          {
             // could also to "tmpfile %s\n" like shmfile but just
             // a mmaped tmp file on the system
             printf("data\n");
             fwrite(data, width * height * sizeof(DATA32), 1, stdout);
          }
        shm_free();
     }
   else
     printf("done\n");

   _gst_shutdown();

   return 0;
}
