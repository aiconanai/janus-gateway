/*! \file   janus_videocall.c
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU Affero General Public License v3
 * \brief  Janus VideoCall plugin
 * \details  This is a simple video call plugin for Janus, allowing two
 * WebRTC peers to call each other through the gateway. The idea is to
 * provide a similar service as the well known AppRTC demo (https://apprtc.appspot.com),
 * but with the media flowing through the gateway rather than being peer-to-peer.
 * 
 * The plugin provides a simple fake registration mechanism. A peer attaching
 * to the plugin needs to specify a username, which acts as a "phone number":
 * if the username is free, it is associated with the peer, which means
 * he/she can be "called" using that username by another peer. Peers can
 * either "call" another peer, by specifying their username, or wait for a call.
 * The approach used by this plugin is similar to the one employed by the
 * echo test one: all frames (RTP/RTCP) coming from one peer are relayed
 * to the other.
 * 
 * Just as in the janus_echotest.c plugin, there are knobs to control
 * whether audio and/or video should be muted or not, and if the bitrate
 * of the peer needs to be capped by means of REMB messages.
 * 
 * \ingroup plugins
 * \ref plugins
 */

#include "plugin.h"

#include <jansson.h>

#include "../config.h"
#include "../rtcp.h"


/* Plugin information */
#define JANUS_VIDEOCALL_VERSION			1
#define JANUS_VIDEOCALL_VERSION_STRING	"0.0.1"
#define JANUS_VIDEOCALL_DESCRIPTION		"This is a simple video call plugin for Janus, allowing two WebRTC peers to call each other through the gateway."
#define JANUS_VIDEOCALL_NAME			"JANUS VideoCall plugin"
#define JANUS_VIDEOCALL_PACKAGE			"janus.plugin.videocall"

/* Plugin methods */
janus_plugin *create(void);
int janus_videocall_init(janus_callbacks *callback, const char *config_path);
void janus_videocall_destroy(void);
int janus_videocall_get_version(void);
const char *janus_videocall_get_version_string(void);
const char *janus_videocall_get_description(void);
const char *janus_videocall_get_name(void);
const char *janus_videocall_get_package(void);
void janus_videocall_create_session(janus_pluginession *handle, int *error);
void janus_videocall_handle_message(janus_pluginession *handle, char *transaction, char *message, char *sdp_type, char *sdp);
void janus_videocall_setup_media(janus_pluginession *handle);
void janus_videocall_incoming_rtp(janus_pluginession *handle, int video, char *buf, int len);
void janus_videocall_incoming_rtcp(janus_pluginession *handle, int video, char *buf, int len);
void janus_videocall_hangup_media(janus_pluginession *handle);
void janus_videocall_destroy_session(janus_pluginession *handle, int *error);

/* Plugin setup */
static janus_plugin janus_videocall_plugin =
	{
		.init = janus_videocall_init,
		.destroy = janus_videocall_destroy,

		.get_version = janus_videocall_get_version,
		.get_version_string = janus_videocall_get_version_string,
		.get_description = janus_videocall_get_description,
		.get_name = janus_videocall_get_name,
		.get_package = janus_videocall_get_package,
		
		.create_session = janus_videocall_create_session,
		.handle_message = janus_videocall_handle_message,
		.setup_media = janus_videocall_setup_media,
		.incoming_rtp = janus_videocall_incoming_rtp,
		.incoming_rtcp = janus_videocall_incoming_rtcp,
		.hangup_media = janus_videocall_hangup_media,
		.destroy_session = janus_videocall_destroy_session,
	}; 

/* Plugin creator */
janus_plugin *create(void) {
	JANUS_PRINT("%s created!\n", JANUS_VIDEOCALL_NAME);
	return &janus_videocall_plugin;
}


/* Useful stuff */
static int initialized = 0, stopping = 0;
static janus_callbacks *gateway = NULL;
static GThread *handler_thread;
static void *janus_videocall_handler(void *data);

typedef struct janus_videocall_message {
	janus_pluginession *handle;
	char *transaction;
	char *message;
	char *sdp_type;
	char *sdp;
} janus_videocall_message;
GQueue *messages;

typedef struct janus_videocall_session {
	janus_pluginession *handle;
	gchar *username;
	gboolean audio_active;
	gboolean video_active;
	uint64_t bitrate;
	struct janus_videocall_session *peer;
	gboolean destroy;
} janus_videocall_session;
GHashTable *sessions;


/* Plugin implementation */
int janus_videocall_init(janus_callbacks *callback, const char *config_path) {
	if(stopping) {
		/* Still stopping from before */
		return -1;
	}
	if(callback == NULL || config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}

	/* Read configuration */
	char filename[255];
	sprintf(filename, "%s/%s.cfg", config_path, JANUS_VIDEOCALL_PACKAGE);
	JANUS_PRINT("Configuration file: %s\n", filename);
	janus_config *config = janus_config_parse(filename);
	if(config != NULL)
		janus_config_print(config);
	/* This plugin actually has nothing to configure... */
	janus_config_destroy(config);
	config = NULL;
	
	sessions = g_hash_table_new(g_str_hash, g_str_equal);
	messages = g_queue_new();
	/* This is the callback we'll need to invoke to contact the gateway */
	gateway = callback;

	initialized = 1;
	/* Launch the thread that will handle incoming messages */
	GError *error = NULL;
	handler_thread = g_thread_try_new("janus videocall handler", janus_videocall_handler, NULL, &error);
	if(error != NULL) {
		initialized = 0;
		/* Something went wrong... */
		JANUS_DEBUG("Got error %d (%s) trying to launch thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	JANUS_PRINT("%s initialized!\n", JANUS_VIDEOCALL_NAME);
	return 0;
}

void janus_videocall_destroy() {
	if(!initialized)
		return;
	stopping = 1;
	if(handler_thread != NULL) {
		g_thread_join(handler_thread);
	}
	handler_thread = NULL;
	/* TODO Actually clean up and remove ongoing sessions */
	g_hash_table_destroy(sessions);
	g_queue_free(messages);
	sessions = NULL;
	initialized = 0;
	stopping = 0;
	JANUS_PRINT("%s destroyed!\n", JANUS_VIDEOCALL_NAME);
}

int janus_videocall_get_version() {
	return JANUS_VIDEOCALL_VERSION;
}

const char *janus_videocall_get_version_string() {
	return JANUS_VIDEOCALL_VERSION_STRING;
}

const char *janus_videocall_get_description() {
	return JANUS_VIDEOCALL_DESCRIPTION;
}

const char *janus_videocall_get_name() {
	return JANUS_VIDEOCALL_NAME;
}

const char *janus_videocall_get_package() {
	return JANUS_VIDEOCALL_PACKAGE;
}

void janus_videocall_create_session(janus_pluginession *handle, int *error) {
	if(stopping || !initialized) {
		*error = -1;
		return;
	}	
	janus_videocall_session *session = (janus_videocall_session *)calloc(1, sizeof(janus_videocall_session));
	if(session == NULL) {
		JANUS_DEBUG("Memory error!\n");
		*error = -2;
		return;
	}
	session->handle = handle;
	session->audio_active = TRUE;
	session->video_active = TRUE;
	session->bitrate = 0;	/* No limit */
	session->peer = NULL;
	session->username = NULL;
	handle->plugin_handle = session;

	return;
}

void janus_videocall_destroy_session(janus_pluginession *handle, int *error) {
	if(stopping || !initialized) {
		*error = -1;
		return;
	}
	janus_videocall_session *session = (janus_videocall_session *)handle->plugin_handle; 
	if(!session) {
		JANUS_DEBUG("No session associated with this handle...\n");
		*error = -2;
		return;
	}
	if(session->destroy) {
		JANUS_PRINT("Session already destroyed...\n");
		g_free(session);
		return;
	}
	JANUS_PRINT("Removing user %s session...\n", session->username ? session->username : "'unknown'");
	janus_videocall_hangup_media(handle);
	session->destroy = TRUE;
	/* FIXME Actually clean up session */
	if(session->username != NULL) {
		JANUS_PRINT("  -- Removed: %d\n", g_hash_table_remove(sessions, (gpointer)session->username));
	}
	g_free(session);
	return;
}

void janus_videocall_handle_message(janus_pluginession *handle, char *transaction, char *message, char *sdp_type, char *sdp) {
	if(stopping || !initialized)
		return;
	JANUS_PRINT("%s\n", message);
	janus_videocall_message *msg = calloc(1, sizeof(janus_videocall_message));
	if(msg == NULL) {
		JANUS_DEBUG("Memory error!\n");
		return;
	}
	msg->handle = handle;
	msg->transaction = transaction ? g_strdup(transaction) : NULL;
	msg->message = message;
	msg->sdp_type = sdp_type;
	msg->sdp = sdp;
	g_queue_push_tail(messages, msg);
}

void janus_videocall_setup_media(janus_pluginession *handle) {
	JANUS_DEBUG("WebRTC media is now available\n");
	if(stopping || !initialized)
		return;
	janus_videocall_session *session = (janus_videocall_session *)handle->plugin_handle;	
	if(!session) {
		JANUS_DEBUG("No session associated with this handle...\n");
		return;
	}
	if(session->destroy)
		return;
	/* We really don't care, as we only relay RTP/RTCP we get in the first place anyway */
}

void janus_videocall_incoming_rtp(janus_pluginession *handle, int video, char *buf, int len) {
	if(stopping || !initialized)
		return;
	if(gateway) {
		/* Honour the audio/video active flags */
		janus_videocall_session *session = (janus_videocall_session *)handle->plugin_handle;	
		if(!session) {
			JANUS_DEBUG("No session associated with this handle...\n");
			return;
		}
		if(!session->peer) {
			JANUS_DEBUG("Session has no peer...\n");
			return;
		}
		if(session->destroy || session->peer->destroy)
			return;
		if((!video && session->audio_active) || (video && session->video_active)) {
			gateway->relay_rtp(session->peer->handle, video, buf, len);
		}
	}
}

void janus_videocall_incoming_rtcp(janus_pluginession *handle, int video, char *buf, int len) {
	if(stopping || !initialized)
		return;
	if(gateway) {
		janus_videocall_session *session = (janus_videocall_session *)handle->plugin_handle;	
		if(!session) {
			JANUS_DEBUG("No session associated with this handle...\n");
			return;
		}
		if(!session->peer) {
			JANUS_DEBUG("Session has no peer...\n");
			return;
		}
		if(session->destroy || session->peer->destroy)
			return;
		if(session->bitrate > 0)
			janus_rtcp_cap_remb(buf, len, session->bitrate);
		gateway->relay_rtcp(session->peer->handle, video, buf, len);
	}
}

void janus_videocall_hangup_media(janus_pluginession *handle) {
	JANUS_PRINT("No WebRTC media anymore\n");
	if(stopping || !initialized)
		return;
	janus_videocall_session *session = (janus_videocall_session *)handle->plugin_handle;	
	if(!session) {
		JANUS_DEBUG("No session associated with this handle...\n");
		return;
	}
	if(session->destroy)
		return;
	if(session->peer) {
		/* Send event to our peer too */
		json_t *call = json_object();
		json_object_set(call, "videocall", json_string("event"));
		json_t *calling = json_object();
		json_object_set_new(calling, "event", json_string("hangup"));
		json_object_set_new(calling, "username", json_string(session->username));
		json_object_set_new(calling, "reason", json_string("Remote hangup"));
		json_object_set_new(call, "result", calling);
		char *call_text = json_dumps(call, JSON_INDENT(3));
		json_decref(call);
		JANUS_PRINT("Pushing event to peer: %s\n", call_text);
		JANUS_PRINT("  >> %d\n", gateway->push_event(session->peer->handle, &janus_videocall_plugin, NULL, call_text, NULL, NULL));
	}
	session->peer = NULL;
	/* Reset controls */
	session->audio_active = TRUE;
	session->video_active = TRUE;
	session->bitrate = 0;
}

/* Thread to handle incoming messages */
static void *janus_videocall_handler(void *data) {
	JANUS_DEBUG("Joining thread\n");
	janus_videocall_message *msg = NULL;
	char *error_cause = calloc(512, sizeof(char));	/* FIXME 512 should be enough, but anyway... */
	if(error_cause == NULL) {
		JANUS_DEBUG("Memory error!\n");
		return NULL;
	}
	while(initialized && !stopping) {
		if(!messages || (msg = g_queue_pop_head(messages)) == NULL) {
			usleep(50000);
			continue;
		}
		janus_videocall_session *session = (janus_videocall_session *)msg->handle->plugin_handle;	
		if(!session) {
			JANUS_DEBUG("No session associated with this handle...\n");
			continue;
		}
		if(session->destroy)
			continue;
		/* Handle request */
		JANUS_PRINT("Handling message: %s\n", msg->message);
		if(msg->message == NULL) {
			JANUS_DEBUG("No message??\n");
			sprintf(error_cause, "%s", "No message??");
			goto error;
		}
		json_error_t error;
		json_t *root = json_loads(msg->message, 0, &error);
		if(!root) {
			JANUS_DEBUG("JSON error: on line %d: %s\n", error.line, error.text);
			sprintf(error_cause, "JSON error: on line %d: %s", error.line, error.text);
			goto error;
		}
		if(!json_is_object(root)) {
			JANUS_DEBUG("JSON error: not an object\n");
			sprintf(error_cause, "JSON error: not an object");
			goto error;
		}
		json_t *request = json_object_get(root, "request");
		if(!request || !json_is_string(request)) {
			JANUS_DEBUG("JSON error: invalid element (request)\n");
			sprintf(error_cause, "JSON error: invalid element (request)");
			goto error;
		}
		const char *request_text = json_string_value(request);
		json_t *result = NULL;
		char *sdp_type = NULL, *sdp = NULL;
		if(!strcasecmp(request_text, "list")) {
			result = json_object();
			json_t *list = json_array();
			JANUS_PRINT("Request for the list of peers\n");
			/* Return a list of all available mountpoints */
			GList *peers_list = g_hash_table_get_values(sessions);
			GList *m = peers_list;
			while(m) {
				janus_videocall_session *user = (janus_videocall_session *)m->data;
				if(user != NULL && user->username != NULL)
					json_array_append_new(list, json_string(user->username));
				m = m->next;
			}
			json_object_set_new(result, "list", list);
			g_list_free(peers_list);
		} else if(!strcasecmp(request_text, "register")) {
			/* Map this handle to a username */
			if(session->username != NULL) {
				JANUS_DEBUG("Already registered (%s)\n", session->username);
				sprintf(error_cause, "Already registered (%s)", session->username);
				goto error;
			}
			json_t *username = json_object_get(root, "username");
			if(!username || !json_is_string(username)) {
				JANUS_DEBUG("JSON error: missing element (username)\n");
				sprintf(error_cause, "JSON error: missing element (username)");
				goto error;
			}
			const char *username_text = json_string_value(username);
			if(g_hash_table_lookup(sessions, username_text) != NULL) {
				JANUS_DEBUG("Username '%s' already taken\n", username_text);
				sprintf(error_cause, "Username '%s' already taken", username_text);
				goto error;
			}
			session->username = g_strdup(username_text);
			if(session->username == NULL) {
				JANUS_DEBUG("Memory error!\n");
				sprintf(error_cause, "Memory error");
				goto error;
			}
			g_hash_table_insert(sessions, (gpointer)session->username, session);
			result = json_object();
			json_object_set_new(result, "event", json_string("registered"));
			json_object_set_new(result, "username", json_string(username_text));
		} else if(!strcasecmp(request_text, "call")) {
			/* Call another peer */
			if(session->peer != NULL) {
				JANUS_DEBUG("Already in a call\n");
				sprintf(error_cause, "Already in a call");
				goto error;
			}
			json_t *username = json_object_get(root, "username");
			if(!username || !json_is_string(username)) {
				JANUS_DEBUG("JSON error: missing element (username)\n");
				sprintf(error_cause, "JSON error: missing element (username)");
				goto error;
			}
			const char *username_text = json_string_value(username);
			janus_videocall_session *peer = g_hash_table_lookup(sessions, username_text);
			if(peer == NULL) {
				JANUS_DEBUG("Username '%s' doesn't exist\n", username_text);
				sprintf(error_cause, "Username '%s' doesn't exist", username_text);
				goto error;
			}
			if(peer->peer != NULL) {
				JANUS_PRINT("%s is busy\n", username_text);
				result = json_object();
				json_object_set_new(result, "event", json_string("hangup"));
				json_object_set_new(result, "username", json_string(session->username));
				json_object_set_new(result, "reason", json_string("User busy"));
			} else {
				/* Any SDP to handle? if not, something's wrong */
				if(!msg->sdp) {
					JANUS_DEBUG("Missing SDP\n");
					sprintf(error_cause, "Missing SDP");
					goto error;
				}
				session->peer = peer;
				peer->peer = session;
				JANUS_PRINT("%s is calling %s\n", session->username, session->peer->username);
				JANUS_PRINT("This is involving a negotiation (%s) as well:\n%s\n", msg->sdp_type, msg->sdp);
				/* Send SDP to our peer */
				json_t *call = json_object();
				json_object_set(call, "videocall", json_string("event"));
				json_t *calling = json_object();
				json_object_set_new(calling, "event", json_string("incomingcall"));
				json_object_set_new(calling, "username", json_string(session->username));
				json_object_set_new(call, "result", calling);
				char *call_text = json_dumps(call, JSON_INDENT(3));
				json_decref(call);
				JANUS_PRINT("Pushing event to peer: %s\n", call_text);
				JANUS_PRINT("  >> %d\n", gateway->push_event(peer->handle, &janus_videocall_plugin, NULL, call_text, msg->sdp_type, msg->sdp));
				/* Send an ack back */
				result = json_object();
				json_object_set_new(result, "event", json_string("calling"));
			}
		} else if(!strcasecmp(request_text, "accept")) {
			/* Accept a call from another peer */
			if(session->peer == NULL) {
				JANUS_DEBUG("No incoming call to accept\n");
				sprintf(error_cause, "No incoming call to accept");
				goto error;
			}
			/* Any SDP to handle? if not, something's wrong */
			if(!msg->sdp) {
				JANUS_DEBUG("Missing SDP\n");
				sprintf(error_cause, "Missing SDP");
				goto error;
			}
			JANUS_PRINT("%s is accepting a call from %s\n", session->username, session->peer->username);
			JANUS_PRINT("This is involving a negotiation (%s) as well:\n%s\n", msg->sdp_type, msg->sdp);
			/* Send SDP to our peer */
			json_t *call = json_object();
			json_object_set(call, "videocall", json_string("event"));
			json_t *calling = json_object();
			json_object_set_new(calling, "event", json_string("accepted"));
			json_object_set_new(calling, "username", json_string(session->username));
			json_object_set_new(call, "result", calling);
			char *call_text = json_dumps(call, JSON_INDENT(3));
			json_decref(call);
			JANUS_PRINT("Pushing event to peer: %s\n", call_text);
			JANUS_PRINT("  >> %d\n", gateway->push_event(session->peer->handle, &janus_videocall_plugin, NULL, call_text, msg->sdp_type, msg->sdp));
			/* Send an ack back */
			result = json_object();
			json_object_set_new(result, "event", json_string("accepted"));
		} else if(!strcasecmp(request_text, "set")) {
			/* Update the local configuration (audio/video mute/unmute, or bitrate cap) */
			json_t *audio = json_object_get(root, "audio");
			if(audio && !json_is_boolean(audio)) {
				JANUS_DEBUG("JSON error: invalid element (audio)\n");
				sprintf(error_cause, "JSON error: invalid value (audio)");
				goto error;
			}
			json_t *video = json_object_get(root, "video");
			if(video && !json_is_boolean(video)) {
				JANUS_DEBUG("JSON error: invalid element (video)\n");
				sprintf(error_cause, "JSON error: invalid value (video)");
				goto error;
			}
			json_t *bitrate = json_object_get(root, "bitrate");
			if(bitrate && !json_is_integer(bitrate)) {
				JANUS_DEBUG("JSON error: invalid element (bitrate)\n");
				sprintf(error_cause, "JSON error: invalid value (bitrate)");
				goto error;
			}
			if(audio) {
				session->audio_active = json_is_true(audio);
				JANUS_PRINT("Setting audio property: %s\n", session->audio_active ? "true" : "false");
			}
			if(video) {
				session->video_active = json_is_true(video);
				JANUS_PRINT("Setting video property: %s\n", session->video_active ? "true" : "false");
			}
			if(bitrate) {
				session->bitrate = json_integer_value(bitrate);
				JANUS_PRINT("Setting video bitrate: %"SCNu64"\n", session->bitrate);
				if(session->bitrate > 0) {
					/* FIXME Generate a new REMB (especially useful for Firefox, which doesn't send any we can cap later) */
					char buf[24];
					memset(buf, 0, 24);
					janus_rtcp_remb((char *)&buf, 24, session->bitrate);
					JANUS_PRINT("Sending REMB\n");
					gateway->relay_rtcp(session->handle, 1, buf, 24);
					/* FIXME How should we handle a subsequent "no limit" bitrate? */
				}
			}
			/* Send an ack back */
			result = json_object();
			json_object_set(result, "event", json_string("set"));
		} else if(!strcasecmp(request_text, "hangup")) {
			/* Hangup an ongoing call or reject an incoming one */
			if(session->peer == NULL) {
				JANUS_DEBUG("No call to hangup\n");
				//~ sprintf(error_cause, "No call to hangup");
				//~ goto error;
				continue;
			}
			janus_videocall_session *peer = session->peer;
			JANUS_PRINT("%s is hanging up the call with %s\n", session->username, peer->username);
			session->peer = NULL;
			peer->peer = NULL;
			/* Notify the success as an hangup message */
			result = json_object();
			json_object_set_new(result, "event", json_string("hangup"));
			json_object_set_new(result, "username", json_string(session->username));
			json_object_set_new(result, "reason", json_string("We did the hangup"));
			/* Send event to our peer too */
			json_t *call = json_object();
			json_object_set(call, "videocall", json_string("event"));
			json_t *calling = json_object();
			json_object_set_new(calling, "event", json_string("hangup"));
			json_object_set_new(calling, "username", json_string(session->username));
			json_object_set_new(calling, "reason", json_string("Remote hangup"));
			json_object_set_new(call, "result", calling);
			char *call_text = json_dumps(call, JSON_INDENT(3));
			json_decref(call);
			JANUS_PRINT("Pushing event to peer: %s\n", call_text);
			JANUS_PRINT("  >> %d\n", gateway->push_event(peer->handle, &janus_videocall_plugin, NULL, call_text, NULL, NULL));
		} else {
			JANUS_DEBUG("Unknown request (%s)\n", request_text);
			sprintf(error_cause, "Unknown request (%s)", request_text);
			goto error;
		}

		json_decref(root);
		/* Prepare JSON event */
		json_t *event = json_object();
		json_object_set(event, "videocall", json_string("event"));
		if(result != NULL)
			json_object_set(event, "result", result);
		char *event_text = json_dumps(event, JSON_INDENT(3));
		json_decref(event);
		if(result != NULL)
			json_decref(result);
		JANUS_PRINT("Pushing event: %s\n", event_text);
		JANUS_PRINT("  >> %d\n", gateway->push_event(msg->handle, &janus_videocall_plugin, msg->transaction, event_text, sdp_type, sdp));
		if(sdp)
			g_free(sdp);
		continue;
		
error:
		{
			if(root != NULL)
				json_decref(root);
			/* Prepare JSON error event */
			json_t *event = json_object();
			json_object_set(event, "videocall", json_string("event"));
			json_object_set(event, "error", json_string(error_cause));
			char *event_text = json_dumps(event, JSON_INDENT(3));
			json_decref(event);
			JANUS_PRINT("Pushing event: %s\n", event_text);
			JANUS_PRINT("  >> %d\n", gateway->push_event(msg->handle, &janus_videocall_plugin, msg->transaction, event_text, NULL, NULL));
		}
	}
	JANUS_DEBUG("Leaving thread\n");
	return NULL;
}
