/**
 * @file purple-media.c
 *
 * pidgin-sipe
 *
 * Copyright (C) 2010 SIPE Project <http://sipe.sourceforge.net/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "glib.h"

#include "sipe-common.h"

#include "mediamanager.h"
#include "request.h"
#include "agent.h"

#include "sipe-backend.h"
#include "sipe-core.h"

#include "purple-private.h"

struct sipe_backend_media {
	PurpleMedia *m;
	// Prevent infinite recursion in on_stream_info_cb
	gboolean in_recursion;
	GSList *streams;
};

struct sipe_backend_stream {
	gchar *sessionid;
	gchar *participant;
};

static PurpleMediaSessionType sipe_media_to_purple(SipeMediaType type);
static PurpleMediaCandidateType sipe_candidate_type_to_purple(SipeCandidateType type);
static PurpleMediaNetworkProtocol sipe_network_protocol_to_purple(SipeNetworkProtocol proto);
static SipeNetworkProtocol purple_network_protocol_to_sipe(PurpleMediaNetworkProtocol proto);

static void
on_candidates_prepared_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media,
			  SIPE_UNUSED_PARAMETER gchar *sessionid,
			  SIPE_UNUSED_PARAMETER gchar *participant,
			  struct sipe_media_call *call)
{
	if (call->candidates_prepared_cb)
		call->candidates_prepared_cb(call);
}

static void
on_state_changed_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media,
		    PurpleMediaState state,
		    gchar *sessionid,
		    gchar *participant,
		    struct sipe_media_call *call)
{
	SIPE_DEBUG_INFO("sipe_media_state_changed_cb: %d %s %s\n", state, sessionid, participant);
	if (state == PURPLE_MEDIA_STATE_CONNECTED && call->media_connected_cb)
		call->media_connected_cb(call);
}

static struct sipe_backend_stream *
session_find(struct sipe_backend_media *media,
	     const gchar *sessionid,
	     const gchar *participant)
{
	GSList *streams = media->streams;

	while (streams) {
		struct sipe_backend_stream *s = streams->data;
		if (   sipe_strequal(s->sessionid, sessionid)
		    && sipe_strequal(s->participant, participant))
			return s;

		streams = streams->next;
	}

	return NULL;
}

static void
on_stream_info_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media,
		  PurpleMediaInfoType type,
		  gchar *sessionid,
		  gchar *participant,
		  gboolean local,
		  struct sipe_media_call *call)
{
	struct sipe_backend_media *m = call->backend_private;
	if (m->in_recursion) {
		m->in_recursion = FALSE;
		return;
	}

	if (type == PURPLE_MEDIA_INFO_ACCEPT && call->call_accept_cb
	    && !sessionid && !participant)
		call->call_accept_cb(call, local);
	else if (type == PURPLE_MEDIA_INFO_HOLD && call->call_hold_cb) {
		call->call_hold_cb(call, local, TRUE);
		if (!local) {
			m->in_recursion = TRUE;
			purple_media_stream_info(m->m, PURPLE_MEDIA_INFO_HOLD, NULL, NULL, TRUE);
		}
	} else if (type == PURPLE_MEDIA_INFO_UNHOLD && call->call_hold_cb) {
		call->call_hold_cb(call, local, FALSE);
		m->in_recursion = TRUE;
		if (!call->local_on_hold && !call->remote_on_hold) {
			purple_media_stream_info(m->m, PURPLE_MEDIA_INFO_UNHOLD, NULL, NULL, TRUE);
		} else {
			/* Remote side is still on hold, keep local also held to prevent sending 
			 * unnecessary media over network */
			purple_media_stream_info(m->m, PURPLE_MEDIA_INFO_HOLD, NULL, NULL, TRUE);
		}
	} else if (type == PURPLE_MEDIA_INFO_HANGUP || type == PURPLE_MEDIA_INFO_REJECT) {
		if (!sessionid && !participant) {
			if (type == PURPLE_MEDIA_INFO_HANGUP && call->call_hangup_cb)
				call->call_hangup_cb(call, local);
			else if (type == PURPLE_MEDIA_INFO_REJECT && call->call_reject_cb)
				call->call_reject_cb(call, local);
		} else if (sessionid && participant) {
			struct sipe_backend_stream *stream;
			stream = session_find(m, sessionid, participant);

			if (stream) {
				m->streams = g_slist_remove(m->streams, stream);
				g_free(stream->participant);
				g_free(stream);
			}
		}
	}
}

struct sipe_backend_media *
sipe_backend_media_new(struct sipe_core_public *sipe_public,
		       struct sipe_media_call *call,
		       const gchar *participant,
		       gboolean initiator)
{
	struct sipe_backend_media *media = g_new0(struct sipe_backend_media, 1);
	struct sipe_backend_private *purple_private = sipe_public->backend_private;
	PurpleMediaManager *manager = purple_media_manager_get();

	media->m = purple_media_manager_create_media(manager,
						     purple_private->account,
						     "fsrtpconference",
						     participant, initiator);

	g_signal_connect(G_OBJECT(media->m), "candidates-prepared",
			 G_CALLBACK(on_candidates_prepared_cb), call);
	g_signal_connect(G_OBJECT(media->m), "stream-info",
			 G_CALLBACK(on_stream_info_cb), call);
	g_signal_connect(G_OBJECT(media->m), "state-changed",
			 G_CALLBACK(on_state_changed_cb), call);

	return media;
}

void
sipe_backend_media_free(struct sipe_backend_media *media)
{
	purple_media_manager_remove_media(purple_media_manager_get(), media->m);
	g_free(media);
}

struct sipe_backend_stream *
sipe_backend_media_add_stream(struct sipe_backend_media *media,
			      const gchar* participant,
			      SipeMediaType type,
			      gboolean use_nice,
			      gboolean initiator)
{
	struct sipe_backend_stream *stream = NULL;
	PurpleMediaSessionType prpl_type = sipe_media_to_purple(type);
	GParameter *params = NULL;
	guint params_cnt = 0;
	gchar *transmitter;
	gchar *sessionid;

	if (use_nice) {
		transmitter = "nice";
		params_cnt = 2;

		params = g_new0(GParameter, params_cnt);
		params[0].name = "controlling-mode";
		g_value_init(&params[0].value, G_TYPE_BOOLEAN);
		g_value_set_boolean(&params[0].value, initiator);
		params[1].name = "compatibility-mode";
		g_value_init(&params[1].value, G_TYPE_UINT);
		g_value_set_uint(&params[1].value, NICE_COMPATIBILITY_OC2007R2);

		sessionid = "sipe-voice-nice";
	} else {
		transmitter = "rawudp";
		sessionid = "sipe-voice-rawudp";
	}

	if (purple_media_add_stream(media->m, sessionid, participant, prpl_type,
				    initiator, transmitter, params_cnt, params)) {
		stream = g_new0(struct sipe_backend_stream, 1);
		stream->sessionid = sessionid;
		stream->participant = g_strdup(participant);

		media->streams = g_slist_append(media->streams, stream);
	}

	return stream;
}

void
sipe_backend_media_remove_stream(struct sipe_backend_media *media, struct sipe_backend_stream *stream)
{
	purple_media_end(media->m, stream->sessionid, stream->participant);
}

void
sipe_backend_media_add_remote_candidates(struct sipe_backend_media *media,
					 struct sipe_backend_stream *stream,
					 GList *candidates)
{
	purple_media_add_remote_candidates(media->m, stream->sessionid,
					   stream->participant, candidates);
}

gboolean sipe_backend_media_is_initiator(struct sipe_backend_media *media,
					 struct sipe_backend_stream *stream)
{
	return purple_media_is_initiator(media->m,
					 stream->sessionid,
					 stream->participant);
}

GList *
sipe_backend_media_get_active_local_candidates(struct sipe_backend_media *media,
					       struct sipe_backend_stream *stream)
{
	return purple_media_get_active_local_candidates(media->m,
							stream->sessionid,
							stream->participant);
}

GList *
sipe_backend_media_get_active_remote_candidates(struct sipe_backend_media *media,
						struct sipe_backend_stream *stream)
{
	return purple_media_get_active_remote_candidates(media->m,
							 stream->sessionid,
							 stream->participant);
}

struct sipe_backend_codec *
sipe_backend_codec_new(int id, const char *name, SipeMediaType type, guint clock_rate)
{
	return (struct sipe_backend_codec *)purple_media_codec_new(id, name,
						    sipe_media_to_purple(type),
						    clock_rate);
}

void
sipe_backend_codec_free(struct sipe_backend_codec *codec)
{
	if (codec)
		g_object_unref(codec);
}

int
sipe_backend_codec_get_id(struct sipe_backend_codec *codec)
{
	return purple_media_codec_get_id((PurpleMediaCodec *)codec);
}

gchar *
sipe_backend_codec_get_name(struct sipe_backend_codec *codec)
{
	return purple_media_codec_get_encoding_name((PurpleMediaCodec *)codec);
}

guint
sipe_backend_codec_get_clock_rate(struct sipe_backend_codec *codec)
{
	return purple_media_codec_get_clock_rate((PurpleMediaCodec *)codec);
}

void
sipe_backend_codec_add_optional_parameter(struct sipe_backend_codec *codec,
										  const gchar *name, const gchar *value)
{
	purple_media_codec_add_optional_parameter((PurpleMediaCodec *)codec, name, value);
}

GList *
sipe_backend_codec_get_optional_parameters(struct sipe_backend_codec *codec)
{
	return purple_media_codec_get_optional_parameters((PurpleMediaCodec *)codec);
}

gboolean
sipe_backend_set_remote_codecs(struct sipe_media_call *call,
			       struct sipe_backend_stream *stream)
{
	return purple_media_set_remote_codecs(call->backend_private->m,
					      stream->sessionid,
					      stream->participant,
					      call->remote_codecs);
}

GList*
sipe_backend_get_local_codecs(struct sipe_media_call *call,
			      struct sipe_backend_stream *stream)
{
	return purple_media_get_codecs(call->backend_private->m, stream->sessionid);
}

struct sipe_backend_candidate *
sipe_backend_candidate_new(const gchar *foundation,
			   SipeComponentType component,
			   SipeCandidateType type, SipeNetworkProtocol proto,
			   const gchar *ip, guint port)
{
	return (struct sipe_backend_candidate *)purple_media_candidate_new(
		foundation,
		component,
		sipe_candidate_type_to_purple(type),
		sipe_network_protocol_to_purple(proto),
		ip,
		port);
}

void
sipe_backend_candidate_free(struct sipe_backend_candidate *candidate)
{
	if (candidate)
		g_object_unref(candidate);
}

gchar *
sipe_backend_candidate_get_username(struct sipe_backend_candidate *candidate)
{
	return purple_media_candidate_get_username((PurpleMediaCandidate*)candidate);
}

gchar *
sipe_backend_candidate_get_password(struct sipe_backend_candidate *candidate)
{
	return purple_media_candidate_get_password((PurpleMediaCandidate*)candidate);
}

gchar *
sipe_backend_candidate_get_foundation(struct sipe_backend_candidate *candidate)
{
	return purple_media_candidate_get_foundation((PurpleMediaCandidate*)candidate);
}

gchar *
sipe_backend_candidate_get_ip(struct sipe_backend_candidate *candidate)
{
	return purple_media_candidate_get_ip((PurpleMediaCandidate*)candidate);
}

guint
sipe_backend_candidate_get_port(struct sipe_backend_candidate *candidate)
{
	return purple_media_candidate_get_port((PurpleMediaCandidate*)candidate);
}

guint32
sipe_backend_candidate_get_priority(struct sipe_backend_candidate *candidate)
{
	return purple_media_candidate_get_priority((PurpleMediaCandidate*)candidate);
}

void
sipe_backend_candidate_set_priority(struct sipe_backend_candidate *candidate, guint32 priority)
{
	g_object_set(candidate, "priority", priority, NULL);
}

SipeComponentType
sipe_backend_candidate_get_component_type(struct sipe_backend_candidate *candidate)
{
	return purple_media_candidate_get_component_id((PurpleMediaCandidate*)candidate);
}

SipeCandidateType
sipe_backend_candidate_get_type(struct sipe_backend_candidate *candidate)
{
	return purple_media_candidate_get_candidate_type((PurpleMediaCandidate*)candidate);
}

SipeNetworkProtocol
sipe_backend_candidate_get_protocol(struct sipe_backend_candidate *candidate)
{
	PurpleMediaNetworkProtocol proto =
		purple_media_candidate_get_protocol((PurpleMediaCandidate*)candidate);
	return purple_network_protocol_to_sipe(proto);
}

void
sipe_backend_candidate_set_username_and_pwd(struct sipe_backend_candidate *candidate,
					    const gchar *username,
					    const gchar *password)
{
	g_object_set(candidate, "username", username, "password", password, NULL);
}

GList*
sipe_backend_get_local_candidates(struct sipe_backend_media *media,
				  struct sipe_backend_stream *stream)
{
	return purple_media_get_local_candidates(media->m, stream->sessionid, stream->participant);
}

void
sipe_backend_media_hold(struct sipe_backend_media *media, gboolean local)
{
	purple_media_stream_info(media->m, PURPLE_MEDIA_INFO_HOLD,
				 NULL, NULL, local);
}

void
sipe_backend_media_unhold(struct sipe_backend_media *media, gboolean local)
{
	purple_media_stream_info(media->m, PURPLE_MEDIA_INFO_UNHOLD,
				 NULL, NULL, local);
}

void
sipe_backend_media_hangup(struct sipe_backend_media *media, gboolean local)
{
	purple_media_stream_info(media->m, PURPLE_MEDIA_INFO_HANGUP,
				 NULL, NULL, local);
}

void
sipe_backend_media_reject(struct sipe_backend_media *media, gboolean local)
{
	purple_media_stream_info(media->m, PURPLE_MEDIA_INFO_REJECT,
				 NULL, NULL, local);
}

static PurpleMediaSessionType sipe_media_to_purple(SipeMediaType type)
{
	switch (type) {
		case SIPE_MEDIA_AUDIO: return PURPLE_MEDIA_AUDIO;
		case SIPE_MEDIA_VIDEO: return PURPLE_MEDIA_VIDEO;
		default:               return PURPLE_MEDIA_NONE;
	}
}

/*SipeMediaType purple_media_to_sipe(PurpleMediaSessionType type)
{
	switch (type) {
		case PURPLE_MEDIA_AUDIO: return SIPE_MEDIA_AUDIO;
		case PURPLE_MEDIA_VIDEO: return SIPE_MEDIA_VIDEO;
		default:				 return SIPE_MEDIA_AUDIO;
	}
}*/

static PurpleMediaCandidateType
sipe_candidate_type_to_purple(SipeCandidateType type)
{
	switch (type) {
		case SIPE_CANDIDATE_TYPE_HOST:	return PURPLE_MEDIA_CANDIDATE_TYPE_HOST;
		case SIPE_CANDIDATE_TYPE_RELAY:	return PURPLE_MEDIA_CANDIDATE_TYPE_RELAY;
		case SIPE_CANDIDATE_TYPE_SRFLX:	return PURPLE_MEDIA_CANDIDATE_TYPE_SRFLX;
		default:						return PURPLE_MEDIA_CANDIDATE_TYPE_HOST;
	}
}

static PurpleMediaNetworkProtocol
sipe_network_protocol_to_purple(SipeNetworkProtocol proto)
{
	switch (proto) {
		case SIPE_NETWORK_PROTOCOL_TCP:	return PURPLE_MEDIA_NETWORK_PROTOCOL_TCP;
		case SIPE_NETWORK_PROTOCOL_UDP:	return PURPLE_MEDIA_NETWORK_PROTOCOL_UDP;
		default:						return PURPLE_MEDIA_NETWORK_PROTOCOL_TCP;
	}
}

static SipeNetworkProtocol
purple_network_protocol_to_sipe(PurpleMediaNetworkProtocol proto)
{
	switch (proto) {
		case PURPLE_MEDIA_NETWORK_PROTOCOL_TCP: return SIPE_NETWORK_PROTOCOL_TCP;
		case PURPLE_MEDIA_NETWORK_PROTOCOL_UDP: return SIPE_NETWORK_PROTOCOL_UDP;
		default:								return SIPE_NETWORK_PROTOCOL_UDP;
	}
}

/*
  Local Variables:
  mode: c
  c-file-style: "bsd"
  indent-tabs-mode: t
  tab-width: 8
  End:
*/