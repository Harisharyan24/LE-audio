
// leaudiod.c — LE Audio LC3 sink daemon using BlueZ MediaEndpoint/Transport
// Build: gcc leaudiod.c lc3_pipe.c -o leaudiod `pkg-config --cflags --libs glib-2.0 gio-2.0 alsa` -llc3
// References: MediaEndpoint(5), MediaTransport(5), BlueZ Media API docs.

#include <gio/gio.h>
#include <glib.h>
#include <alsa/asoundlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "lc3_pipe.h"

#define BLUEZ_BUS          "org.bluez"
#define ADAPTER_PATH       "/org/bluez/hci0"
#define MEDIA_IFACE        "org.bluez.Media1"
#define ENDPOINT_IFACE     "org.bluez.MediaEndpoint1"
#define TRANSPORT_IFACE    "org.bluez.MediaTransport1"

#define BAP_SINK_UUID      "00002bc9-0000-1000-8000-00805f9b34fb"  // Unicast Sink
#define LC3_CODEC_ID       0x06

static GDBusConnection *bus = NULL;
static char *transport_path = NULL;
static int iso_fd = -1;
static GThread *rx_thread = NULL;
static volatile gboolean run_rx = FALSE;

// ALSA simple sink (48kHz mono/stereo as configured)
static snd_pcm_t *pcm = NULL;

// Configure ALSA sink
static gboolean alsa_open(unsigned rate, unsigned channels)
{
    int err;
    if ((err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0)
        return FALSE;

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_rate(pcm, hw, rate, 0);
    snd_pcm_hw_params_set_channels(pcm, hw, channels);
    snd_pcm_hw_params(pcm, hw);
    snd_pcm_prepare(pcm);
    return TRUE;
}

static void alsa_close() {
    if (pcm) { snd_pcm_close(pcm); pcm = NULL; }
}

// RX thread: read LC3 frames from ISO fd, decode to PCM, write to ALSA
static gpointer rx_loop(gpointer unused)
{
    lc3_decoder_t dec = lc3_decoder_create(/* sample_rate */ 48000, /* frame_ms */ 10.0, /* channels */ 1);
    size_t lc3_bytes = lc3_frame_bytes(dec);
    int16_t pcm_buf[lc3_samples_per_frame(dec)];

    while (run_rx) {
        uint8_t frame[512];
        ssize_t n = read(iso_fd, frame, lc3_bytes);
        if (n <= 0) break; // stopped/error
        lc3_decode(dec, frame, n, pcm_buf);
        snd_pcm_sframes_t wr = snd_pcm_writei(pcm, pcm_buf, lc3_samples_per_frame(dec));
        if (wr < 0) snd_pcm_prepare(pcm);
    }
    lc3_decoder_destroy(dec);
    return NULL;
}

// --- MediaEndpoint methods ---

// SelectProperties: choose LC3 configuration and QoS
static gboolean on_select_props(GDBusConnection *c, const char *sender,
    const char *obj, const char *iface, const char *method,
    GVariant *params, GDBusMethodInvocation *inv, gpointer user)
{
    // params: (a{sv}) capabilities from peer
    // Return (ay ay a{sv}) -> Capabilities, Metadata, QoS selection.
    // LC3 capability TLV must be constructed per PAC; here is a minimal placeholder.

    const uint8_t lc3_caps[] = {
        // Example TLV structure placeholder; fill per BAP LC3 PAC (sampling freq, frame dur, octets)
        0x02, 0x06, /* ... */
    };
    const uint8_t metadata[] = { /* optional metadata; can be empty */ };

    GVariantBuilder qos;
    g_variant_builder_init(&qos, G_VARIANT_TYPE("a{sv}"));
    // Example QoS (tune for your device):
    g_variant_builder_add(&qos, "{sv}", "Framing",            g_variant_new_byte(0x00)); // unframed
    g_variant_builder_add(&qos, "{sv}", "PHY",                g_variant_new_byte(0x02)); // 2M PHY
    g_variant_builder_add(&qos, "{sv}", "MaximumLatency",     g_variant_new_uint16(20)); // ms
    g_variant_builder_add(&qos, "{sv}", "PreferredMinimumDelay", g_variant_new_uint32(20));
    g_variant_builder_add(&qos, "{sv}", "PreferredMaximumDelay", g_variant_new_uint32(40));

    g_dbus_method_invocation_return_value(inv,
        g_variant_new("(ayay@a{sv})",
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, lc3_caps, sizeof(lc3_caps), sizeof(uint8_t)),
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, metadata, sizeof(metadata), sizeof(uint8_t)),
            g_variant_builder_end(&qos)));

    return TRUE;
}

// SetConfiguration: grab transport and start the stream
static gboolean on_set_config(GDBusConnection *c, const char *sender,
    const char *obj, const char *iface, const char *method,
    GVariant *params, GDBusMethodInvocation *inv, gpointer user)
{
    const char *tpath; GVariant *props;
    g_variant_get(params, "(&o@a{sv})", &tpath, &props);
    transport_path = g_strdup(tpath);

    // Acquire ISO fd
    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync(c, BLUEZ_BUS, transport_path,
        TRANSPORT_IFACE, "Acquire", NULL, G_VARIANT_TYPE("(hqq)"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (err) {
        g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Failed", err->message);
        g_error_free(err);
        return TRUE;
    }
    gint32 fd_idx; guint16 rmtu, wmtu;
    g_variant_get(ret, "(hqq)", &fd_idx, &rmtu, &wmtu);
    iso_fd = g_dbus_connection_dup_fd(c, fd_idx, NULL);
    g_variant_unref(ret);

    // Open ALSA (48kHz, 1ch) and spawn RX
    alsa_open(48000, 1);
    run_rx = TRUE;
    rx_thread = g_thread_new("rx", rx_loop, NULL);

    g_dbus_method_invocation_return_value(inv, NULL);
    return TRUE;
}

static gboolean on_clear_config(GDBusConnection *c, const char *sender,
    const char *obj, const char *iface, const char *method,
    GVariant *params, GDBusMethodInvocation *inv, gpointer user)
{
    run_rx = FALSE;
    if (rx_thread) { g_thread_join(rx_thread); rx_thread = NULL; }
    alsa_close();
    if (iso_fd >= 0) { close(iso_fd); iso_fd = -1; }
    g_clear_pointer(&transport_path, g_free);
    g_dbus_method_invocation_return_value(inv, NULL);
    return TRUE;
}

static gboolean on_release(GDBusConnection *c, const char *sender,
    const char *obj, const char *iface, const char *method,
    GVariant *params, GDBusMethodInvocation *inv, gpointer user)
{
    run_rx = FALSE;
    if (rx_thread) { g_thread_join(rx_thread); rx_thread = NULL; }
    alsa_close();
    if (iso_fd >= 0) { close(iso_fd); iso_fd = -1; }
    g_dbus_method_invocation_return_value(inv, NULL);
    return TRUE;
}

// Register our endpoint with org.bluez.Media1
static void register_endpoint()
{
    // Export endpoint object
    const char *xml =
        "<node>"
        " <interface name='org.bluez.MediaEndpoint1'>"
        "  <method name='SelectProperties'>"
        "    <arg type='a{sv}' direction='in'/>"
        "    <arg type='ay'   direction='out'/>"
        "    <arg type='ay'   direction='out'/>"
        "    <arg type='a{sv}' direction='out'/>"
        "  </method>"
        "  <method name='SetConfiguration'>"
        "    <arg type='o' direction='in'/>"
        "    <arg type='a{sv}' direction='in'/>"
        "  </method>"
        "  <method name='ClearConfiguration'>"
        "    <arg type='o' direction='in'/>"
        "  </method>"
        "  <method name='Release'/>"
        " </interface>"
        "</node>";

    GDBusNodeInfo *ni = g_dbus_node_info_new_for_xml(xml, NULL);
    static const GDBusInterfaceVTable vtable = {
        .method_call =  {
            if (!g_strcmp0(method, "SelectProperties"))  on_select_props(c, sender, obj, iface, method, params, inv, u);
            else if (!g_strcmp0(method, "SetConfiguration")) on_set_config(c, sender, obj, iface, method, params, inv, u);
            else if (!g_strcmp0(method, "ClearConfiguration")) on_clear_config(c, sender, obj, iface, method, params, inv, u);
            else if (!g_strcmp0(method, "Release")) on_release(c, sender, obj, iface, method, params, inv, u);
        }
    };

    GError *err = NULL;
    const char *endpoint_path = "/leaudio/endpoint0";
    g_dbus_connection_register_object(bus, endpoint_path, ni->interfaces[0], &vtable, NULL, NULL, &err);
    if (err) g_error("Endpoint export failed: %s", err->message);
    g_dbus_node_info_unref(ni);

    // Prepare RegisterEndpoint properties
    GVariantBuilder caps;
    g_variant_builder_init(&caps, G_VARIANT_TYPE("ay"));
    // TODO: fill LC3 PAC capability bytes per BAP spec (sampling freq, frame dur, octets, channels)
    g_variant_builder_add(&caps, "y", (guchar) LC3_CODEC_ID);

    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "UUID",      g_variant_new_string(BAP_SINK_UUID));
    g_variant_builder_add(&props, "{sv}", "Codec",     g_variant_new_byte(LC3_CODEC_ID));
    g_variant_builder_add(&props, "{sv}", "Capabilities", g_variant_builder_end(&caps));

    // Register with org.bluez.Media1 on the adapter
    g_dbus_connection_call_sync(bus, BLUEZ_BUS, ADAPTER_PATH, MEDIA_IFACE, "RegisterEndpoint",
        g_variant_new("(oa{sv})", endpoint_path, g_variant_builder_end(&props)),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (err) g_error("RegisterEndpoint failed: %s", err->message);
}

int main()
{
    GError *err = NULL;
    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (err) g_error("Failed to get system bus: %s", err->message);

    register_endpoint();

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
