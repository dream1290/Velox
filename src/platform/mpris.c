/* vlx_mpris.c — MPRIS2 D-Bus implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implements:
 *   org.mpris.MediaPlayer2        — identity, quit, raise
 *   org.mpris.MediaPlayer2.Player — transport, position, volume, metadata
 *
 * This gives Velox compatibility with: media keys, Playerctl,
 * KDE Connect, and any MPRIS2-aware desktop widget.
 */

#include "platform/mpris.h"
#include "core/event_bus.h"
#include "utils/log.h"

#include <gio/gio.h>

#define MPRIS_BUS_NAME      "org.mpris.MediaPlayer2.velox"
#define MPRIS_OBJ_PATH      "/org/mpris/MediaPlayer2"
#define MPRIS_IFACE_ROOT    "org.mpris.MediaPlayer2"
#define MPRIS_IFACE_PLAYER  "org.mpris.MediaPlayer2.Player"

/* Introspection XML — minimal but standards-compliant */
static const gchar MPRIS_XML[] =
"<node>"
"  <interface name='" MPRIS_IFACE_ROOT "'>"
"    <property name='Identity'        type='s' access='read'/>"
"    <property name='DesktopEntry'    type='s' access='read'/>"
"    <property name='CanQuit'         type='b' access='read'/>"
"    <property name='CanRaise'        type='b' access='read'/>"
"    <property name='HasTrackList'    type='b' access='read'/>"
"    <method   name='Raise'/>"
"    <method   name='Quit'/>"
"  </interface>"
"  <interface name='" MPRIS_IFACE_PLAYER "'>"
"    <property name='PlaybackStatus'  type='s' access='read'/>"
"    <property name='LoopStatus'      type='s' access='readwrite'/>"
"    <property name='Rate'            type='d' access='readwrite'/>"
"    <property name='Shuffle'         type='b' access='readwrite'/>"
"    <property name='Volume'          type='d' access='readwrite'/>"
"    <property name='Position'        type='x' access='read'/>"
"    <property name='MinimumRate'     type='d' access='read'/>"
"    <property name='MaximumRate'     type='d' access='read'/>"
"    <property name='CanGoNext'       type='b' access='read'/>"
"    <property name='CanGoPrevious'   type='b' access='read'/>"
"    <property name='CanPlay'         type='b' access='read'/>"
"    <property name='CanPause'        type='b' access='read'/>"
"    <property name='CanSeek'         type='b' access='read'/>"
"    <property name='CanControl'      type='b' access='read'/>"
"    <property name='Metadata'        type='a{sv}' access='read'/>"
"    <method   name='Next'/>"
"    <method   name='Previous'/>"
"    <method   name='Pause'/>"
"    <method   name='PlayPause'/>"
"    <method   name='Stop'/>"
"    <method   name='Play'/>"
"    <method   name='Seek'>"
"      <arg direction='in' type='x' name='Offset'/>"
"    </method>"
"    <method   name='SetPosition'>"
"      <arg direction='in' type='o' name='TrackId'/>"
"      <arg direction='in' type='x' name='Position'/>"
"    </method>"
"    <signal   name='Seeked'>"
"      <arg type='x' name='Position'/>"
"    </signal>"
"  </interface>"
"</node>";

struct _VlxMprisProvider {
    GObject     parent_instance;

    VlxPlayer  *player;
    VlxEventBus *bus;

    guint        bus_name_id;
    guint        root_reg_id;
    guint        player_reg_id;
    GDBusConnection *connection;

    VlxPlayerState  state;
    gint64          position_us;
    gint64          duration_us;
    gchar          *title;
    gchar          *uri;
    gdouble         volume;
};

G_DEFINE_TYPE (VlxMprisProvider, vlx_mpris_provider, G_TYPE_OBJECT)

/* ── Property getters ────────────────────────────────────────────────── */
static const gchar *
state_to_string (VlxPlayerState s)
{
    switch (s) {
    case VLX_STATE_PLAYING: return "Playing";
    case VLX_STATE_PAUSED:  return "Paused";
    default:                return "Stopped";
    }
}

static GVariant *
build_metadata (VlxMprisProvider *self)
{
    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));

    g_variant_builder_add (&b, "{sv}", "mpris:trackid",
        g_variant_new_object_path (
            self->uri ? "/io/github/velox/track/1"
                      : "/org/mpris/MediaPlayer2/TrackList/NoTrack"));

    if (self->duration_us > 0)
        g_variant_builder_add (&b, "{sv}", "mpris:length",
            g_variant_new_int64 (self->duration_us));

    if (self->title)
        g_variant_builder_add (&b, "{sv}", "xesam:title",
            g_variant_new_string (self->title));

    if (self->uri)
        g_variant_builder_add (&b, "{sv}", "xesam:url",
            g_variant_new_string (self->uri));

    return g_variant_builder_end (&b);
}

/* ── D-Bus method/property handlers ─────────────────────────────────── */
static void
handle_method_root (GDBusConnection       *conn,
                    const gchar           *sender,
                    const gchar           *obj_path,
                    const gchar           *iface,
                    const gchar           *method,
                    GVariant              *params,
                    GDBusMethodInvocation *invocation,
                    gpointer               data)
{
    (void) conn; (void) sender; (void) obj_path;
    (void) iface; (void) params;
    VlxMprisProvider *self = VLX_MPRIS_PROVIDER (data);
    (void) self;

    if (g_strcmp0 (method, "Quit") == 0) {
        g_application_quit (g_application_get_default ());
    } else if (g_strcmp0 (method, "Raise") == 0) {
        /* Raise window — handled by application */
    }
    g_dbus_method_invocation_return_value (invocation, NULL);
}

static GVariant *
handle_get_root (GDBusConnection *conn,
                 const gchar     *sender,
                 const gchar     *obj_path,
                 const gchar     *iface,
                 const gchar     *prop,
                 GError         **error,
                 gpointer         data)
{
    (void) conn; (void) sender; (void) obj_path; (void) iface; (void) data;
    (void) error;

    if (g_strcmp0 (prop, "Identity")     == 0) return g_variant_new_string ("Velox");
    if (g_strcmp0 (prop, "DesktopEntry") == 0) return g_variant_new_string ("io.github.velox");
    if (g_strcmp0 (prop, "CanQuit")      == 0) return g_variant_new_boolean (TRUE);
    if (g_strcmp0 (prop, "CanRaise")     == 0) return g_variant_new_boolean (TRUE);
    if (g_strcmp0 (prop, "HasTrackList") == 0) return g_variant_new_boolean (FALSE);
    return NULL;
}

static void
handle_method_player (GDBusConnection       *conn,
                       const gchar           *sender,
                       const gchar           *obj_path,
                       const gchar           *iface,
                       const gchar           *method,
                       GVariant              *params,
                       GDBusMethodInvocation *invocation,
                       gpointer               data)
{
    (void) conn; (void) sender; (void) obj_path; (void) iface;
    VlxMprisProvider *self = VLX_MPRIS_PROVIDER (data);

    if      (g_strcmp0 (method, "Play")      == 0) vlx_player_play   (self->player);
    else if (g_strcmp0 (method, "Pause")     == 0) vlx_player_pause  (self->player);
    else if (g_strcmp0 (method, "PlayPause") == 0) vlx_player_toggle (self->player);
    else if (g_strcmp0 (method, "Stop")      == 0) vlx_player_stop   (self->player);
    else if (g_strcmp0 (method, "Next")      == 0) { /* Playlist unsupported */ }
    else if (g_strcmp0 (method, "Previous")  == 0) { /* Playlist unsupported */ }
    else if (g_strcmp0 (method, "Seek")      == 0) {
        gint64 offset;
        g_variant_get (params, "(x)", &offset);
        vlx_player_seek_relative (self->player, offset);
    } else if (g_strcmp0 (method, "SetPosition") == 0) {
        const gchar *track_id;
        gint64 pos;
        g_variant_get (params, "(&ox)", &track_id, &pos);
        vlx_player_seek (self->player, pos);
    }

    g_dbus_method_invocation_return_value (invocation, NULL);
}

static GVariant *
handle_get_player (GDBusConnection *conn,
                   const gchar     *sender,
                   const gchar     *obj_path,
                   const gchar     *iface,
                   const gchar     *prop,
                   GError         **error,
                   gpointer         data)
{
    (void) conn; (void) sender; (void) obj_path; (void) iface; (void) error;
    VlxMprisProvider *self = VLX_MPRIS_PROVIDER (data);

    if (g_strcmp0 (prop, "PlaybackStatus") == 0)
        return g_variant_new_string (state_to_string (self->state));
    if (g_strcmp0 (prop, "LoopStatus")    == 0) return g_variant_new_string ("None");
    if (g_strcmp0 (prop, "Rate")          == 0)
        return g_variant_new_double (vlx_player_get_rate (self->player));
    if (g_strcmp0 (prop, "Shuffle")       == 0) return g_variant_new_boolean (FALSE);
    if (g_strcmp0 (prop, "Volume")        == 0)
        return g_variant_new_double (self->volume);
    if (g_strcmp0 (prop, "Position")      == 0)
        return g_variant_new_int64 (self->position_us);
    if (g_strcmp0 (prop, "MinimumRate")   == 0) return g_variant_new_double (0.25);
    if (g_strcmp0 (prop, "MaximumRate")   == 0) return g_variant_new_double (4.0);
    if (g_strcmp0 (prop, "CanGoNext")     == 0) return g_variant_new_boolean (FALSE);
    if (g_strcmp0 (prop, "CanGoPrevious") == 0) return g_variant_new_boolean (FALSE);
    if (g_strcmp0 (prop, "CanPlay")       == 0) return g_variant_new_boolean (TRUE);
    if (g_strcmp0 (prop, "CanPause")      == 0) return g_variant_new_boolean (TRUE);
    if (g_strcmp0 (prop, "CanSeek")       == 0) return g_variant_new_boolean (TRUE);
    if (g_strcmp0 (prop, "CanControl")    == 0) return g_variant_new_boolean (TRUE);
    if (g_strcmp0 (prop, "Metadata")      == 0) return build_metadata (self);
    return NULL;
}

static const GDBusInterfaceVTable root_vtable = {
    handle_method_root, handle_get_root, NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};
static const GDBusInterfaceVTable player_vtable = {
    handle_method_player, handle_get_player, NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

/* ── Bus acquired ────────────────────────────────────────────────────── */
static void
on_bus_acquired (GDBusConnection *conn,
                 const gchar     *name,
                 gpointer         data)
{
    VlxMprisProvider *self = VLX_MPRIS_PROVIDER (data);
    (void) name;

    g_set_object (&self->connection, conn);

    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml (MPRIS_XML, NULL);

    self->root_reg_id = g_dbus_connection_register_object (
        conn, MPRIS_OBJ_PATH,
        g_dbus_node_info_lookup_interface (node, MPRIS_IFACE_ROOT),
        &root_vtable, self, NULL, NULL);

    self->player_reg_id = g_dbus_connection_register_object (
        conn, MPRIS_OBJ_PATH,
        g_dbus_node_info_lookup_interface (node, MPRIS_IFACE_PLAYER),
        &player_vtable, self, NULL, NULL);

    g_dbus_node_info_unref (node);
    VLX_LOG_INFO (VLX_LOG_DOMAIN_PLATFORM, "MPRIS2 registered on D-Bus");
}

/* ── Event bus → MPRIS property change notifications ────────────────── */
static void
emit_properties_changed (VlxMprisProvider *self,
                          const gchar      *iface,
                          GVariant         *changed_props)
{
    if (!self->connection) return;

    GVariantBuilder inv;
    g_variant_builder_init (&inv, G_VARIANT_TYPE ("as"));

    g_dbus_connection_emit_signal (
        self->connection,
        NULL, MPRIS_OBJ_PATH,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        g_variant_new ("(s@a{sv}@as)",
                       iface,
                       changed_props,
                       g_variant_builder_end (&inv)),
        NULL);
}

static void
on_state_changed (VlxEventBus *bus, VlxPlayerState state, gpointer data)
{
    VlxMprisProvider *self = VLX_MPRIS_PROVIDER (data);
    (void) bus;
    self->state = state;

    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&b, "{sv}", "PlaybackStatus",
                           g_variant_new_string (state_to_string (state)));
    emit_properties_changed (self, MPRIS_IFACE_PLAYER,
                             g_variant_builder_end (&b));
}

static void
on_position_updated (VlxEventBus *bus,
                     gint64 pos_us, gint64 dur_us,
                     gpointer data)
{
    VlxMprisProvider *self = VLX_MPRIS_PROVIDER (data);
    (void) bus;
    self->position_us = pos_us;
    self->duration_us = dur_us;
}

static void
on_volume_changed (VlxEventBus *bus, gdouble vol, gpointer data)
{
    VlxMprisProvider *self = VLX_MPRIS_PROVIDER (data);
    (void) bus;
    self->volume = vol;

    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&b, "{sv}", "Volume", g_variant_new_double (vol));
    emit_properties_changed (self, MPRIS_IFACE_PLAYER,
                             g_variant_builder_end (&b));
}

static void
on_media_info (VlxEventBus *bus,
               const gchar *title, const gchar *uri, gint64 dur_us,
               gpointer data)
{
    VlxMprisProvider *self = VLX_MPRIS_PROVIDER (data);
    (void) bus;

    g_free (self->title);
    g_free (self->uri);
    self->title       = g_strdup (title);
    self->uri         = g_strdup (uri);
    self->duration_us = dur_us;

    GVariantBuilder b;
    g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&b, "{sv}", "Metadata", build_metadata (self));
    emit_properties_changed (self, MPRIS_IFACE_PLAYER,
                             g_variant_builder_end (&b));
}

/* ── GObject boilerplate ─────────────────────────────────────────────── */
static void
vlx_mpris_provider_finalize (GObject *obj)
{
    VlxMprisProvider *self = VLX_MPRIS_PROVIDER (obj);
    vlx_mpris_provider_stop (self);
    g_signal_handlers_disconnect_by_data (self->bus, self);
    g_free (self->title);
    g_free (self->uri);
    g_clear_object (&self->player);
    G_OBJECT_CLASS (vlx_mpris_provider_parent_class)->finalize (obj);
}

static void
vlx_mpris_provider_class_init (VlxMprisProviderClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = vlx_mpris_provider_finalize;
}

static void
vlx_mpris_provider_init (VlxMprisProvider *self)
{
    self->volume = 1.0;
    self->state  = VLX_STATE_IDLE;
}

VlxMprisProvider *
vlx_mpris_provider_new (VlxPlayer *player)
{
    g_return_val_if_fail (VLX_IS_PLAYER (player), NULL);

    VlxMprisProvider *self = g_object_new (VLX_TYPE_MPRIS_PROVIDER, NULL);
    self->player = g_object_ref (player);
    self->bus    = vlx_event_bus_get_default ();

    g_signal_connect (self->bus, "state-changed",
                      G_CALLBACK (on_state_changed), self);
    g_signal_connect (self->bus, "position-updated",
                      G_CALLBACK (on_position_updated), self);
    g_signal_connect (self->bus, "volume-changed",
                      G_CALLBACK (on_volume_changed), self);
    g_signal_connect (self->bus, "media-info",
                      G_CALLBACK (on_media_info), self);
    return self;
}

void
vlx_mpris_provider_start (VlxMprisProvider *self)
{
    g_return_if_fail (VLX_IS_MPRIS_PROVIDER (self));
    if (self->bus_name_id != 0) return;

    self->bus_name_id = g_bus_own_name (
        G_BUS_TYPE_SESSION,
        MPRIS_BUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired,
        NULL, NULL,
        self, NULL);
}

void
vlx_mpris_provider_stop (VlxMprisProvider *self)
{
    g_return_if_fail (VLX_IS_MPRIS_PROVIDER (self));

    if (self->bus_name_id) {
        g_bus_unown_name (self->bus_name_id);
        self->bus_name_id = 0;
    }
    if (self->connection && self->root_reg_id) {
        g_dbus_connection_unregister_object (self->connection,
                                             self->root_reg_id);
        g_dbus_connection_unregister_object (self->connection,
                                             self->player_reg_id);
        self->root_reg_id = 0;
        self->player_reg_id = 0;
    }
    g_clear_object (&self->connection);
}
