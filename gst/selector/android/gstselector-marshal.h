
#ifndef __gst_selector_marshal_MARSHAL_H__
#define __gst_selector_marshal_MARSHAL_H__

#include	<glib-object.h>

G_BEGIN_DECLS

/* INT64:VOID (gstselector-marshal.list:1) */
extern void gst_selector_marshal_INT64__VOID (GClosure     *closure,
                                              GValue       *return_value,
                                              guint         n_param_values,
                                              const GValue *param_values,
                                              gpointer      invocation_hint,
                                              gpointer      marshal_data);

/* VOID:OBJECT,INT64,INT64 (gstselector-marshal.list:2) */
extern void gst_selector_marshal_VOID__OBJECT_INT64_INT64 (GClosure     *closure,
                                                           GValue       *return_value,
                                                           guint         n_param_values,
                                                           const GValue *param_values,
                                                           gpointer      invocation_hint,
                                                           gpointer      marshal_data);

G_END_DECLS

#endif /* __gst_selector_marshal_MARSHAL_H__ */

