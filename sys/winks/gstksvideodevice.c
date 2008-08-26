/*
 * Copyright (C) 2008 Ole André Vadla Ravnås <ole.andre.ravnas@tandberg.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "gstksvideodevice.h"

#include "gstksclock.h"
#include "kshelpers.h"
#include "ksvideohelpers.h"

#define READ_TIMEOUT           (10 * 1000)
#define MJPEG_MAX_PADDING      128
#define MAX_OUTSTANDING_FRAMES 128
#define BUFFER_ALIGNMENT       512

#define DEFAULT_DEVICE_PATH NULL

GST_DEBUG_CATEGORY_EXTERN (gst_ks_debug);
#define GST_CAT_DEFAULT gst_ks_debug

enum
{
  PROP_0,
  PROP_CLOCK,
  PROP_DEVICE_PATH,
};

typedef struct
{
  KSSTREAM_HEADER header;
  KS_FRAME_INFO frame_info;
} KSSTREAM_READ_PARAMS;

typedef struct
{
  KSSTREAM_READ_PARAMS params;
  guint8 *buf_unaligned;
  guint8 *buf;
  OVERLAPPED overlapped;
} ReadRequest;

typedef struct
{
  gboolean open;
  KSSTATE state;

  GstKsClock *clock;
  gchar *dev_path;
  HANDLE filter_handle;
  GList *media_types;
  GstCaps *cached_caps;
  HANDLE cancel_event;

  KsVideoMediaType *cur_media_type;
  GstCaps *cur_fixed_caps;
  guint width;
  guint height;
  guint fps_n;
  guint fps_d;
  guint8 *rgb_swap_buf;
  gboolean is_mjpeg;

  HANDLE pin_handle;

  gboolean requests_submitted;
  gulong num_requests;
  GArray *requests;
  GArray *request_events;
} GstKsVideoDevicePrivate;

#define GST_KS_VIDEO_DEVICE_GET_PRIVATE(o) \
    (G_TYPE_INSTANCE_GET_PRIVATE ((o), GST_TYPE_KS_VIDEO_DEVICE, \
    GstKsVideoDevicePrivate))

static void gst_ks_video_device_dispose (GObject * object);
static void gst_ks_video_device_finalize (GObject * object);
static void gst_ks_video_device_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_ks_video_device_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);

static void gst_ks_video_device_reset_caps (GstKsVideoDevice * self);

GST_BOILERPLATE (GstKsVideoDevice, gst_ks_video_device, GObject, G_TYPE_OBJECT);

static void
gst_ks_video_device_base_init (gpointer gclass)
{
}

static void
gst_ks_video_device_class_init (GstKsVideoDeviceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GstKsVideoDevicePrivate));

  gobject_class->dispose = gst_ks_video_device_dispose;
  gobject_class->finalize = gst_ks_video_device_finalize;
  gobject_class->get_property = gst_ks_video_device_get_property;
  gobject_class->set_property = gst_ks_video_device_set_property;

  g_object_class_install_property (gobject_class, PROP_CLOCK,
      g_param_spec_object ("clock", "Clock to use",
          "Clock to use", GST_TYPE_KS_CLOCK,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (gobject_class, PROP_DEVICE_PATH,
      g_param_spec_string ("device-path", "Device Path",
          "The device path", DEFAULT_DEVICE_PATH,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
gst_ks_video_device_init (GstKsVideoDevice * self,
    GstKsVideoDeviceClass * gclass)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  priv->open = FALSE;
  priv->state = KSSTATE_STOP;
}

static void
gst_ks_video_device_dispose (GObject * object)
{
  GstKsVideoDevice *self = GST_KS_VIDEO_DEVICE (object);
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  gst_ks_video_device_reset_caps (self);
  gst_ks_video_device_close (self);

  if (priv->clock != NULL) {
    g_object_unref (priv->clock);
    priv->clock = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_ks_video_device_finalize (GObject * object)
{
  GstKsVideoDevice *self = GST_KS_VIDEO_DEVICE (object);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_ks_video_device_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstKsVideoDevice *self = GST_KS_VIDEO_DEVICE (object);
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  switch (prop_id) {
    case PROP_CLOCK:
      g_value_set_object (value, priv->clock);
      break;
    case PROP_DEVICE_PATH:
      g_value_set_string (value, priv->dev_path);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_ks_video_device_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstKsVideoDevice *self = GST_KS_VIDEO_DEVICE (object);
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  switch (prop_id) {
    case PROP_CLOCK:
      if (priv->clock != NULL)
        g_object_unref (priv->clock);
      priv->clock = g_value_dup_object (value);
      break;
    case PROP_DEVICE_PATH:
      g_free (priv->dev_path);
      priv->dev_path = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_ks_video_device_parse_win32_error (const gchar * func_name,
    DWORD error_code, gulong * ret_error_code, gchar ** ret_error_str)
{
  if (ret_error_code != NULL)
    *ret_error_code = error_code;

  if (ret_error_str != NULL) {
    GString *message;
    gchar buf[1480];
    DWORD result;

    message = g_string_sized_new (1600);
    g_string_append_printf (message, "%s returned ", func_name);

    result =
        FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error_code, 0, buf, sizeof (buf),
        NULL);
    if (result != 0) {
      g_string_append_printf (message, "0x%08x: %s", error_code,
          g_strchomp (buf));
    } else {
      DWORD format_error_code = GetLastError ();

      g_string_append_printf (message,
          "<0x%08x (FormatMessage error code: %s)>", error_code,
          (format_error_code == ERROR_MR_MID_NOT_FOUND)
          ? "no system error message found"
          : "failed to retrieve system error message");
    }

    *ret_error_str = message->str;
    g_string_free (message, FALSE);
  }
}

static void
gst_ks_video_device_clear_buffers (GstKsVideoDevice * self)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);
  guint i;

  if (priv->requests == NULL)
    return;

  /* Cancel pending requests */
  CancelIo (priv->pin_handle);

  for (i = 0; i < priv->num_requests; i++) {
    ReadRequest *req = &g_array_index (priv->requests, ReadRequest, i);
    DWORD bytes_returned;

    GetOverlappedResult (priv->pin_handle, &req->overlapped, &bytes_returned,
        TRUE);
  }

  /* Clean up */
  for (i = 0; i < priv->requests->len; i++) {
    ReadRequest *req = &g_array_index (priv->requests, ReadRequest, i);
    HANDLE ev = g_array_index (priv->request_events, HANDLE, i);

    g_free (req->buf_unaligned);

    if (ev)
      CloseHandle (ev);
  }

  g_array_free (priv->requests, TRUE);
  priv->requests = NULL;

  g_array_free (priv->request_events, TRUE);
  priv->request_events = NULL;
}

static void
gst_ks_video_device_prepare_buffers (GstKsVideoDevice * self)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);
  guint i;
  guint frame_size;

  g_assert (priv->cur_media_type != NULL);

  gst_ks_video_device_clear_buffers (self);

  priv->requests = g_array_sized_new (FALSE, TRUE, sizeof (ReadRequest),
      priv->num_requests);
  priv->request_events = g_array_sized_new (FALSE, TRUE, sizeof (HANDLE),
      priv->num_requests + 1);

  frame_size = gst_ks_video_device_get_frame_size (self);

  for (i = 0; i < priv->num_requests; i++) {
    ReadRequest req = { 0, };

    req.buf_unaligned = g_malloc (frame_size + BUFFER_ALIGNMENT - 1);
    req.buf = (guint8 *) (((gsize) req.buf_unaligned + BUFFER_ALIGNMENT - 1)
        & ~(BUFFER_ALIGNMENT - 1));

    req.overlapped.hEvent = CreateEvent (NULL, TRUE, FALSE, NULL);

    g_array_append_val (priv->requests, req);
    g_array_append_val (priv->request_events, req.overlapped.hEvent);
  }

  g_array_append_val (priv->request_events, priv->cancel_event);
}

static void
gst_ks_video_device_dump_supported_property_sets (GstKsVideoDevice * self,
    const gchar * obj_name, const GUID * propsets, gulong propsets_len)
{
  guint i;

  GST_DEBUG ("%s supports %d property set%s", obj_name, propsets_len,
      (propsets_len != 1) ? "s" : "");

  for (i = 0; i < propsets_len; i++) {
    gchar *propset_name = ks_property_set_to_string (&propsets[i]);
    GST_DEBUG ("[%d] %s", i, propset_name);
    g_free (propset_name);
  }
}

gboolean
gst_ks_video_device_open (GstKsVideoDevice * self)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);
  GUID *propsets = NULL;
  gulong propsets_len;
  GList *cur;

  g_assert (!priv->open);
  g_assert (priv->dev_path != NULL);

  /*
   * Open the filter.
   */
  priv->filter_handle = CreateFile (priv->dev_path,
      GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
  if (!ks_is_valid_handle (priv->filter_handle))
    goto error;

  /*
   * Query the filter for supported property sets.
   */
  if (ks_object_get_supported_property_sets (priv->filter_handle, &propsets,
          &propsets_len)) {
    gst_ks_video_device_dump_supported_property_sets (self, "filter",
        propsets, propsets_len);
    g_free (propsets);
  } else {
    GST_DEBUG ("failed to query filter for supported property sets");
  }

  /*
   * Probe for supported media types.
   */
  priv->media_types = ks_video_probe_filter_for_caps (priv->filter_handle);
  priv->cached_caps = gst_caps_new_empty ();

  for (cur = priv->media_types; cur != NULL; cur = cur->next) {
    KsVideoMediaType *media_type = cur->data;

    gst_caps_append (priv->cached_caps,
        gst_caps_copy (media_type->translated_caps));

#if 1
    {
      gchar *str;
      str = gst_caps_to_string (media_type->translated_caps);
      GST_DEBUG ("pin[%d]: found media type: %s", media_type->pin_id, str);
      g_free (str);
    }
#endif
  }

  priv->cancel_event = CreateEvent (NULL, TRUE, FALSE, NULL);

  priv->open = TRUE;

  return TRUE;

error:
  g_free (priv->dev_path);
  priv->dev_path = NULL;

  return FALSE;
}

void
gst_ks_video_device_close (GstKsVideoDevice * self)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);
  GList *cur;

  gst_ks_video_device_reset_caps (self);

  g_free (priv->dev_path);
  priv->dev_path = NULL;

  if (ks_is_valid_handle (priv->filter_handle)) {
    CloseHandle (priv->filter_handle);
    priv->filter_handle = INVALID_HANDLE_VALUE;
  }

  for (cur = priv->media_types; cur != NULL; cur = cur->next) {
    KsVideoMediaType *mt = cur->data;
    ks_video_media_type_free (mt);
  }

  if (priv->media_types != NULL) {
    g_list_free (priv->media_types);
    priv->media_types = NULL;
  }

  if (priv->cached_caps != NULL) {
    gst_caps_unref (priv->cached_caps);
    priv->cached_caps = NULL;
  }

  if (ks_is_valid_handle (priv->cancel_event))
    CloseHandle (priv->cancel_event);
  priv->cancel_event = INVALID_HANDLE_VALUE;

  priv->open = FALSE;
}

GstCaps *
gst_ks_video_device_get_available_caps (GstKsVideoDevice * self)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  g_assert (priv->open);

  return gst_caps_ref (priv->cached_caps);
}

gboolean
gst_ks_video_device_has_caps (GstKsVideoDevice * self)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  return (priv->cur_media_type != NULL) ? TRUE : FALSE;
}

static HANDLE
gst_ks_video_device_create_pin (GstKsVideoDevice * self,
    KsVideoMediaType * media_type, gulong * num_outstanding)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  HANDLE pin_handle = INVALID_HANDLE_VALUE;
  KSPIN_CONNECT *pin_conn = NULL;
  DWORD ret;

  GUID *propsets = NULL;
  gulong propsets_len;
  gboolean supports_mem_transport = FALSE;

  KSALLOCATOR_FRAMING *framing = NULL;
  gulong framing_size = sizeof (KSALLOCATOR_FRAMING);
  KSALLOCATOR_FRAMING_EX *framing_ex = NULL;
  gulong alignment;

  DWORD mem_transport;

  /*
   * Instantiate the pin.
   */
  pin_conn = ks_video_create_pin_conn_from_media_type (media_type);

  GST_DEBUG ("calling KsCreatePin with pin_id = %d", media_type->pin_id);

  ret = KsCreatePin (priv->filter_handle, pin_conn, GENERIC_READ, &pin_handle);
  if (ret != ERROR_SUCCESS)
    goto error_create_pin;

  GST_DEBUG ("KsCreatePin succeeded, pin %p created", pin_handle);

  g_free (pin_conn);
  pin_conn = NULL;

  /*
   * Query the pin for supported property sets.
   */
  if (ks_object_get_supported_property_sets (pin_handle, &propsets,
          &propsets_len)) {
    guint i;

    gst_ks_video_device_dump_supported_property_sets (self, "pin", propsets,
        propsets_len);

    for (i = 0; i < propsets_len; i++) {
      if (IsEqualGUID (&propsets[i], &KSPROPSETID_MemoryTransport))
        supports_mem_transport = TRUE;
    }

    g_free (propsets);
  } else {
    GST_DEBUG ("failed to query pin for supported property sets");
  }

  /*
   * Figure out how many simultanous requests it prefers.
   *
   * This is really important as it depends on the driver and the device.
   * Doing too few will result in poor capture performance, whilst doing too
   * many will make some drivers crash really horribly and leave you with a
   * BSOD. I've experienced the latter with older Logitech drivers.
   */
  *num_outstanding = 0;
  alignment = 0;

  if (ks_object_get_property (pin_handle, KSPROPSETID_Connection,
          KSPROPERTY_CONNECTION_ALLOCATORFRAMING_EX, &framing_ex, NULL)) {
    if (framing_ex->CountItems >= 1) {
      *num_outstanding = framing_ex->FramingItem[0].Frames;
      alignment = framing_ex->FramingItem[0].FileAlignment;
    } else {
      GST_DEBUG ("ignoring empty ALLOCATORFRAMING_EX");
    }
  } else {
    GST_DEBUG ("query for ALLOCATORFRAMING_EX failed, trying "
        "ALLOCATORFRAMING");

    if (ks_object_get_property (pin_handle, KSPROPSETID_Connection,
            KSPROPERTY_CONNECTION_ALLOCATORFRAMING, &framing, &framing_size)) {
      *num_outstanding = framing->Frames;
      alignment = framing->FileAlignment;
    } else {
      GST_DEBUG ("query for ALLOCATORFRAMING failed");
    }
  }

  GST_DEBUG ("num_outstanding: %d alignment: 0x%08x", *num_outstanding,
      alignment);

  if (*num_outstanding == 0 || *num_outstanding > MAX_OUTSTANDING_FRAMES) {
    GST_DEBUG ("setting number of allowable outstanding frames to 1");
    *num_outstanding = 1;
  }

  g_free (framing);
  g_free (framing_ex);

  /*
   * TODO: We also need to respect alignment, but for now we just align
   *       on FILE_512_BYTE_ALIGNMENT.
   */

  /* Set the memory transport to use. */
  if (supports_mem_transport) {
    mem_transport = 0;          /* REVISIT: use the constant here */
    if (!ks_object_set_property (pin_handle, KSPROPSETID_MemoryTransport,
            KSPROPERTY_MEMORY_TRANSPORT, &mem_transport,
            sizeof (mem_transport))) {
      GST_DEBUG ("failed to set memory transport, sticking with the default");
    }
  }

  /*
   * Override the clock if we have one.
   */
  if (priv->clock != NULL) {
    HANDLE clock_handle = gst_ks_clock_get_handle (priv->clock);

    if (ks_object_set_property (pin_handle, KSPROPSETID_Stream,
            KSPROPERTY_STREAM_MASTERCLOCK, &clock_handle,
            sizeof (clock_handle))) {
      gst_ks_clock_prepare (priv->clock);
    } else {
      GST_WARNING ("failed to set pin's master clock");
    }
  }

  return pin_handle;

  /* ERRORS */
error_create_pin:
  {
    gchar *str;

    gst_ks_video_device_parse_win32_error ("KsCreatePin", ret, NULL, &str);
    GST_ERROR ("%s", str);
    g_free (str);

    goto beach;
  }
beach:
  {
    g_free (framing);
    if (ks_is_valid_handle (pin_handle))
      CloseHandle (pin_handle);
    g_free (pin_conn);

    return INVALID_HANDLE_VALUE;
  }
}

static void
gst_ks_video_device_close_current_pin (GstKsVideoDevice * self)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  if (!ks_is_valid_handle (priv->pin_handle))
    return;

  gst_ks_video_device_set_state (self, KSSTATE_STOP);

  CloseHandle (priv->pin_handle);
  priv->pin_handle = INVALID_HANDLE_VALUE;
}

static void
gst_ks_video_device_reset_caps (GstKsVideoDevice * self)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  gst_ks_video_device_close_current_pin (self);

  ks_video_media_type_free (priv->cur_media_type);
  priv->cur_media_type = NULL;

  priv->width = priv->height = priv->fps_n = priv->fps_d = 0;

  g_free (priv->rgb_swap_buf);
  priv->rgb_swap_buf = NULL;

  if (priv->cur_fixed_caps != NULL) {
    gst_caps_unref (priv->cur_fixed_caps);
    priv->cur_fixed_caps = NULL;
  }
}

gboolean
gst_ks_video_device_set_caps (GstKsVideoDevice * self, GstCaps * caps)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);
  GList *cur;
  GstStructure *s;

  /* State to be committed on success */
  KsVideoMediaType *media_type = NULL;
  guint width, height, fps_n, fps_d;
  HANDLE pin_handle = INVALID_HANDLE_VALUE;

  /* Reset? */
  if (caps == NULL) {
    gst_ks_video_device_reset_caps (self);
    return TRUE;
  }

  /* Validate the caps */
  if (!gst_caps_is_subset (caps, priv->cached_caps)) {
    gchar *string_caps = gst_caps_to_string (caps);
    gchar *string_c_caps = gst_caps_to_string (priv->cached_caps);

    GST_ERROR ("caps (%s) is not a subset of device caps (%s)",
        string_caps, string_c_caps);

    g_free (string_caps);
    g_free (string_c_caps);

    goto error;
  }

  for (cur = priv->media_types; cur != NULL; cur = cur->next) {
    KsVideoMediaType *mt = cur->data;

    if (gst_caps_is_subset (caps, mt->translated_caps)) {
      media_type = ks_video_media_type_dup (mt);
      break;
    }
  }

  if (media_type == NULL)
    goto error;

  s = gst_caps_get_structure (caps, 0);
  if (!gst_structure_get_int (s, "width", &width) ||
      !gst_structure_get_int (s, "height", &height) ||
      !gst_structure_get_fraction (s, "framerate", &fps_n, &fps_d)) {
    GST_ERROR ("Failed to get width/height/fps");
    goto error;
  }

  if (!ks_video_fixate_media_type (media_type->range,
          media_type->format, width, height, fps_n, fps_d))
    goto error;

  if (priv->cur_media_type != NULL) {
    if (media_type->format_size == priv->cur_media_type->format_size &&
        memcmp (media_type->format, priv->cur_media_type->format,
            priv->cur_media_type->format_size) == 0) {
      GST_DEBUG ("%s: re-using existing pin", G_STRFUNC);
      goto same_caps;
    } else {
      GST_DEBUG ("%s: re-creating pin", G_STRFUNC);
    }
  }

  gst_ks_video_device_close_current_pin (self);

  pin_handle = gst_ks_video_device_create_pin (self, media_type,
      &priv->num_requests);
  if (!ks_is_valid_handle (pin_handle)) {
    /* Re-create the old pin */
    if (priv->cur_media_type != NULL)
      priv->pin_handle = gst_ks_video_device_create_pin (self,
          priv->cur_media_type, &priv->num_requests);
    goto error;
  }

  /* Commit state: no turning back past this */
  gst_ks_video_device_reset_caps (self);

  priv->cur_media_type = media_type;
  priv->width = width;
  priv->height = height;
  priv->fps_n = fps_n;
  priv->fps_d = fps_d;

  if (gst_structure_has_name (s, "video/x-raw-rgb"))
    priv->rgb_swap_buf = g_malloc (media_type->sample_size / priv->height);
  else
    priv->rgb_swap_buf = NULL;

  priv->is_mjpeg = gst_structure_has_name (s, "image/jpeg");

  priv->pin_handle = pin_handle;

  priv->cur_fixed_caps = gst_caps_copy (caps);

  return TRUE;

error:
  {
    ks_video_media_type_free (media_type);
    return FALSE;
  }
same_caps:
  {
    ks_video_media_type_free (media_type);
    return TRUE;
  }
}

gboolean
gst_ks_video_device_set_state (GstKsVideoDevice * self, KSSTATE state)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);
  KSSTATE initial_state;
  gint addend;

  g_assert (priv->cur_media_type != NULL);

  if (state == priv->state)
    return TRUE;

  initial_state = priv->state;
  addend = (state > priv->state) ? 1 : -1;

  GST_DEBUG ("Initiating pin state change from %s to %s",
      ks_state_to_string (priv->state), ks_state_to_string (state));

  while (priv->state != state) {
    KSSTATE next_state = priv->state + addend;

    GST_DEBUG ("Changing pin state from %s to %s",
        ks_state_to_string (priv->state), ks_state_to_string (next_state));

    if (ks_object_set_connection_state (priv->pin_handle, next_state)) {
      priv->state = next_state;

      GST_DEBUG ("Changed pin state to %s", ks_state_to_string (priv->state));

      if (priv->state == KSSTATE_PAUSE && addend > 0)
        gst_ks_video_device_prepare_buffers (self);
      else if (priv->state == KSSTATE_ACQUIRE && addend < 0)
        gst_ks_video_device_clear_buffers (self);
    } else {
      GST_WARNING ("Failed to change pin state to %s",
          ks_state_to_string (next_state));
      return FALSE;
    }
  }

  GST_DEBUG ("Finished pin state change from %s to %s",
      ks_state_to_string (initial_state), ks_state_to_string (state));

  return TRUE;
}

guint
gst_ks_video_device_get_frame_size (GstKsVideoDevice * self)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  g_assert (priv->cur_media_type != NULL);

  return priv->cur_media_type->sample_size;
}

GstClockTime
gst_ks_video_device_get_duration (GstKsVideoDevice * self)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  g_assert (priv->cur_media_type != NULL);

  return gst_util_uint64_scale_int (GST_SECOND, priv->fps_d, priv->fps_n);
}

gboolean
gst_ks_video_device_get_latency (GstKsVideoDevice * self,
    GstClockTime * min_latency, GstClockTime * max_latency)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  if (priv->cur_media_type == NULL)
    return FALSE;

  *min_latency =
      gst_util_uint64_scale_int (GST_SECOND, priv->fps_d, priv->fps_n);
  *max_latency = *min_latency;

  return TRUE;
}

static gboolean
gst_ks_video_device_request_frame (GstKsVideoDevice * self, ReadRequest * req,
    gulong * error_code, gchar ** error_str)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);
  HANDLE event;
  KSSTREAM_READ_PARAMS *params;
  BOOL success;
  DWORD bytes_returned = 0;

  /* Reset the OVERLAPPED structure */
  event = req->overlapped.hEvent;
  memset (&req->overlapped, 0, sizeof (OVERLAPPED));
  req->overlapped.hEvent = event;

  /* Fill out KSSTREAM_HEADER and KS_FRAME_INFO */
  params = &req->params;
  memset (params, 0, sizeof (KSSTREAM_READ_PARAMS));

  params->header.Size = sizeof (KSSTREAM_HEADER) + sizeof (KS_FRAME_INFO);
  params->header.PresentationTime.Numerator = 1;
  params->header.PresentationTime.Denominator = 1;
  params->header.FrameExtent = gst_ks_video_device_get_frame_size (self);
  params->header.Data = req->buf;
  params->frame_info.ExtendedHeaderSize = sizeof (KS_FRAME_INFO);

  success = DeviceIoControl (priv->pin_handle, IOCTL_KS_READ_STREAM, NULL, 0,
      params, params->header.Size, &bytes_returned, &req->overlapped);
  if (!success && GetLastError () != ERROR_IO_PENDING)
    goto error_ioctl;

  return TRUE;

  /* ERRORS */
error_ioctl:
  {
    gst_ks_video_device_parse_win32_error ("DeviceIoControl", GetLastError (),
        error_code, error_str);
    return FALSE;
  }
}

GstFlowReturn
gst_ks_video_device_read_frame (GstKsVideoDevice * self, guint8 * buf,
    gulong buf_size, gulong * bytes_read, GstClockTime * presentation_time,
    gulong * error_code, gchar ** error_str)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);
  guint req_idx;
  DWORD wait_ret;
  BOOL success;
  DWORD bytes_returned;

  g_assert (priv->cur_media_type != NULL);

  /* Set the state if needed */
  if (G_UNLIKELY (priv->state != KSSTATE_RUN)) {
    if (priv->clock != NULL)
      gst_ks_clock_start (priv->clock);

    if (!gst_ks_video_device_set_state (self, KSSTATE_RUN))
      goto error_set_state;
  }

  /* First time we're called, submit the requests. */
  if (G_UNLIKELY (!priv->requests_submitted)) {
    priv->requests_submitted = TRUE;

    for (req_idx = 0; req_idx < priv->num_requests; req_idx++) {
      ReadRequest *req = &g_array_index (priv->requests, ReadRequest, req_idx);

      if (!gst_ks_video_device_request_frame (self, req, error_code, error_str))
        goto error_request_failed;
    }
  }

  do {
    /* Wait for either a request to complete, a cancel or a timeout */
    wait_ret = WaitForMultipleObjects (priv->request_events->len,
        (HANDLE *) priv->request_events->data, FALSE, READ_TIMEOUT);
    if (wait_ret == WAIT_TIMEOUT)
      goto error_timeout;
    else if (wait_ret == WAIT_FAILED)
      goto error_wait;

    /* Stopped? */
    if (WaitForSingleObject (priv->cancel_event, 0) == WAIT_OBJECT_0)
      goto error_cancel;

    *bytes_read = 0;

    /* Find the last ReadRequest that finished and get the result, immediately
     * re-issuing each request that has completed. */
    for (req_idx = wait_ret - WAIT_OBJECT_0;
        req_idx < priv->num_requests; req_idx++) {
      ReadRequest *req = &g_array_index (priv->requests, ReadRequest, req_idx);

      /*
       * Completed? WaitForMultipleObjects() returns the lowest index if
       * multiple objects are in the signaled state, and we know that requests
       * are processed one by one so there's no point in looking further once
       * we've found the first that's non-signaled.
       */
      if (WaitForSingleObject (req->overlapped.hEvent, 0) != WAIT_OBJECT_0)
        break;

      success = GetOverlappedResult (priv->pin_handle, &req->overlapped,
          &bytes_returned, TRUE);

      ResetEvent (req->overlapped.hEvent);

      if (success) {
        /* Grab the frame data */
        g_assert (buf_size >= req->params.header.DataUsed);
        memcpy (buf, req->buf, req->params.header.DataUsed);
        *bytes_read = req->params.header.DataUsed;
        if (req->params.header.PresentationTime.Time != 0)
          *presentation_time = req->params.header.PresentationTime.Time * 100;
        else
          *presentation_time = GST_CLOCK_TIME_NONE;

        if (priv->is_mjpeg) {
          /*
           * Workaround for cameras/drivers that intermittently provide us with
           * incomplete or corrupted MJPEG frames.
           *
           * Happens with for instance Microsoft LifeCam VX-7000.
           */

          gboolean valid = FALSE;
          guint padding = 0;

          /* JFIF SOI marker */
          if (*bytes_read > MJPEG_MAX_PADDING
              && buf[0] == 0xff && buf[1] == 0xd8) {
            guint8 *p = buf + *bytes_read - 2;

            /* JFIF EOI marker (but skip any padding) */
            while (padding < MJPEG_MAX_PADDING - 1 - 2 && !valid) {
              if (p[0] == 0xff && p[1] == 0xd9) {
                valid = TRUE;
              } else {
                padding++;
                p--;
              }
            }
          }

          if (valid)
            *bytes_read -= padding;
          else
            *bytes_read = 0;
        }
      } else if (GetLastError () != ERROR_OPERATION_ABORTED)
        goto error_get_result;

      /* Submit a new request immediately */
      if (!gst_ks_video_device_request_frame (self, req, error_code, error_str))
        goto error_request_failed;
    }
  } while (*bytes_read == 0);

  return GST_FLOW_OK;

  /* ERRORS */
error_set_state:
  {
    gst_ks_video_device_parse_win32_error ("gst_ks_video_device_set_state",
        GetLastError (), error_code, error_str);

    return GST_FLOW_ERROR;
  }
error_request_failed:
  {
    return GST_FLOW_ERROR;
  }
error_timeout:
  {
    GST_DEBUG ("IOCTL_KS_READ_STREAM timed out");

    if (error_code != NULL)
      *error_code = 0;
    if (error_str != NULL)
      *error_str = NULL;

    return GST_FLOW_UNEXPECTED;
  }
error_wait:
  {
    gst_ks_video_device_parse_win32_error ("WaitForMultipleObjects",
        GetLastError (), error_code, error_str);

    return GST_FLOW_ERROR;
  }
error_cancel:
  {
    if (error_code != NULL)
      *error_code = 0;
    if (error_str != NULL)
      *error_str = NULL;

    return GST_FLOW_WRONG_STATE;
  }
error_get_result:
  {
    gst_ks_video_device_parse_win32_error ("GetOverlappedResult",
        GetLastError (), error_code, error_str);

    return GST_FLOW_ERROR;
  }
}

void
gst_ks_video_device_postprocess_frame (GstKsVideoDevice * self,
    guint8 * buf, guint buf_size)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  /* If it's RGB we need to flip the image */
  if (priv->rgb_swap_buf != NULL) {
    gint stride, line;
    guint8 *dst, *src;

    stride = buf_size / priv->height;
    dst = buf;
    src = buf + buf_size - stride;

    for (line = 0; line < priv->height / 2; line++) {
      memcpy (priv->rgb_swap_buf, dst, stride);

      memcpy (dst, src, stride);
      memcpy (src, priv->rgb_swap_buf, stride);

      dst += stride;
      src -= stride;
    }
  }
}

void
gst_ks_video_device_cancel (GstKsVideoDevice * self)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  SetEvent (priv->cancel_event);
}

void
gst_ks_video_device_cancel_stop (GstKsVideoDevice * self)
{
  GstKsVideoDevicePrivate *priv = GST_KS_VIDEO_DEVICE_GET_PRIVATE (self);

  ResetEvent (priv->cancel_event);
}