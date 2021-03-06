/*
 * This library is licensed under 2 different licenses and you
 * can choose to use it under the terms of either one of them. The
 * two licenses are the MPL 1.1 and the LGPL.
 *
 * MPL:
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * LGPL:
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
 *
 * The Original Code is Fluendo MPEG Demuxer plugin.
 *
 * The Initial Developer of the Original Code is Fluendo, S.L.
 * Portions created by Fluendo, S.L. are Copyright (C) 2005
 * Fluendo, S.L. All Rights Reserved.
 *
 * Contributor(s): Jan Schmidt <jan@fluendo.com>
 */

#ifndef __FLUTS_PMT_STREAM_INFO_H__
#define __FLUTS_PMT_STREAM_INFO_H__

#include <glib.h>

G_BEGIN_DECLS


typedef struct FluTsPmtStreamInfoClass {
  GObjectClass parent_class;
} FluTsPmtStreamInfoClass;

typedef struct FluTsPmtStreamInfo {
  GObject parent;

  guint16 pid;
  GValueArray *languages; /* null terminated 3 character ISO639 language code */
  guint8 stream_type;
  GValueArray *descriptors;
} FluTsPmtStreamInfo;

FluTsPmtStreamInfo *fluts_pmt_stream_info_new (guint16 pid, guint8 type);
void fluts_pmt_stream_info_add_language(FluTsPmtStreamInfo* si,
    gchar* language);
void fluts_pmt_stream_info_add_descriptor (FluTsPmtStreamInfo *pmt_info,
    const gchar *descriptor, guint length);

GType fluts_pmt_stream_info_get_type (void);

#define FLUTS_TYPE_PMT_STREAM_INFO (fluts_pmt_stream_info_get_type ())

#define FLUTS_IS_PMT_STREAM_INFO(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), FLUTS_TYPE_PMT_STREAM_INFO))
#define FLUTS_PMT_STREAM_INFO(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),FLUTS_TYPE_PMT_STREAM_INFO, FluTsPmtStreamInfo))

G_END_DECLS

#endif
