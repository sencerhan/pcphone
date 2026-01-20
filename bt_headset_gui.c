/*
 * Bluetooth Kulaklık Simülatörü - Pasif Mod
 * Telefon bağlanır, PC sadece kabul eder
 * Otomatik eşleşme + otomatik bağlanma
 *
 * Derleme: make gui
 * Çalıştırma: sudo ./bt_headset_gui
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <gio/gio.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sco.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <pulse/simple.h>
#include <pulse/error.h>

#ifdef HAVE_WEBRTC_APM
#include "audio_processing_wrapper.h"
#endif

// SCO voice ayarları (kernel'de tanımlı değilse)
#ifndef BT_VOICE
#define BT_VOICE 11
struct bt_voice {
    uint16_t setting;
};
#define BT_VOICE_TRANSPARENT 0x0003
#define BT_VOICE_CVSD_16BIT  0x0060
#endif

// Forward declarations
static gboolean sco_connect(void);

// ============================================================================
// STATE MACHINE
// ============================================================================

typedef enum {
    STATE_IDLE,
    STATE_DISCOVERABLE,
    STATE_PAIRING,
    STATE_PAIRED,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_ERROR
} AppState;

typedef enum {
    CALL_IDLE,
    CALL_RINGING,
    CALL_OUTGOING,
    CALL_ACTIVE
} CallState;

static const char* state_names[] = {
    "IDLE", "DISCOVERABLE", "PAIRING", "PAIRED", "CONNECTING", "CONNECTED", "ERROR"
};

// ============================================================================
// GLOBALS
// ============================================================================

static AppState current_state = STATE_IDLE;
static char error_msg[256] = {0};

static CallState current_call_state = CALL_IDLE;
static char current_call_number[64] = {0};
static char current_call_name[128] = {0};
static guint ringtone_timer_id = 0;
static int hfp_socket = -1;  // HFP RFCOMM socket (arama sırasında açık kalır)
static int hfp_listen_socket = -1;  // Gelen aramaları dinlemek için
static int sco_socket = -1;  // SCO audio socket
static pthread_t sco_playback_thread;
static pthread_t sco_capture_thread;
static gboolean sco_audio_running = FALSE;
static pa_simple *pulse_playback = NULL;
static pa_simple *pulse_capture = NULL;
static GThread *incoming_call_thread = NULL;
static gboolean incoming_call_running = FALSE;
static gboolean hfp_listen_paused = FALSE;

// WebRTC AEC
#define AEC_FRAME_SAMPLES 80  // 10ms @ 8kHz
#define AEC_FRAME_BYTES (AEC_FRAME_SAMPLES * 2)
#define AEC_FIFO_CAPACITY (AEC_FRAME_SAMPLES * 50)

static gboolean aec_enabled = FALSE;
#ifdef HAVE_WEBRTC_APM
static gboolean aec_force_disable = TRUE;
#endif
#ifdef HAVE_WEBRTC_APM
static AecHandle *aec_handle = NULL;
#endif
static pthread_mutex_t aec_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t aec_fifo_mutex = PTHREAD_MUTEX_INITIALIZER;
static int16_t aec_render_fifo[AEC_FIFO_CAPACITY];
static int aec_fifo_head = 0;
static int aec_fifo_tail = 0;
static int aec_fifo_size = 0;

static GDBusConnection *dbus_conn = NULL;
static GDBusConnection *obex_conn = NULL;
static char adapter_path[256] = "/org/bluez/hci0";
static guint agent_registration_id = 0;

static char device_path[256] = {0};
static char device_addr[18] = {0};
static char device_name[248] = {0};
static gboolean device_paired = FALSE;

static gboolean auto_connect_in_progress = FALSE;

// Dinamik HFP kanal ve SCO MTU
static uint8_t hfp_channel = 0;  // 0 = henüz bulunmadı
static int sco_mtu = 48;  // Varsayılan, bağlantıda güncellenir

// Contacts
typedef struct {
    char name[128];
    char number[64];
} Contact;

static Contact contacts[200];
static int contacts_count = 0;

typedef struct {
    char type[24];
    char name[128];
    char number[64];
    char time[64];
    char raw_time[20];  // Sıralama için: 20260120T031500
} RecentEntry;

static RecentEntry recent_entries[500];
static int recent_count = 0;

// Son görüşmeleri zamana göre sırala (en yeniden en eskiye)
static int compare_recents(const void *a, const void *b) {
    const RecentEntry *ra = (const RecentEntry *)a;
    const RecentEntry *rb = (const RecentEntry *)b;
    return strcmp(rb->raw_time, ra->raw_time);  // Azalan sıra (en yeni önce)
}

// UI
static GtkWidget *window;
static GtkWidget *state_label;
static GtkWidget *info_label;
static GtkWidget *start_btn;
static GtkWidget *stop_btn;
static GtkWidget *disconnect_btn;
static GtkWidget *answer_btn;
static GtkWidget *reject_btn;
static GtkWidget *hangup_btn;
static GtkWidget *test_call_btn;
static GtkWidget *sync_recents_btn;
static GtkWidget *contacts_spinner;
static GtkWidget *recents_spinner;
static GtkWidget *contacts_search_entry;
static GtkWidget *call_status_label;
static GtkWidget *contacts_view;
static GtkListStore *contacts_store;
static GtkWidget *recent_view;
static GtkListStore *recent_store;
static GtkWidget *log_view;
static GtkTextBuffer *log_buffer;
static GtkWidget *spinner;

// Sync state
static gboolean syncing_contacts = FALSE;
static gboolean syncing_recents = FALSE;
static gchar *pending_search_query = NULL;
static guint search_timeout_id = 0;

// Cached full phonebook (tüm rehber bir kez çekilir)
static Contact all_contacts[2000];
static int all_contacts_count = 0;
static gboolean phonebook_loaded = FALSE;

// CSV dosya yolları
#define CONTACTS_CSV "contacts.csv"
#define RECENTS_CSV "recents.csv"
#define SETTINGS_JSON "settings.json"

// Sütun genişlikleri (varsayılan değerler)
static int col_recent_type = 80;
static int col_recent_name = 150;
static int col_recent_number = 120;
static int col_recent_time = 140;
static int col_contacts_name = 200;
static int col_contacts_number = 150;

// Forward declarations
static void update_ui(void);
static void update_call_ui(void);
static void clear_call_info(void);
static gboolean parse_ciev(const char *buf, int *indicator, int *value);
static void handle_ciev_event(int ind, int val);
static void stop_sco_audio(const char *reason);
static void clear_device_info(void);
static void cleanup_connection(const char *reason, gboolean clear_device);
static void start_incoming_call_listener(void);
static void stop_incoming_call_listener(void);
static gboolean ensure_obexd_running(void);
static gpointer sync_recents_thread(gpointer data);
static gpointer load_phonebook_thread(gpointer data);
static void set_call_state(CallState new_state);
static void log_msg(const char *msg);

// ============================================================================
// SDP - HFP KANAL BULMA
// ============================================================================

// SDP sorgusu ile HFP-AG kanalını bul
static uint8_t find_hfp_channel(const char *addr) {
    bdaddr_t target;
    str2ba(addr, &target);
    
    // SDP session aç
    sdp_session_t *session = sdp_connect(BDADDR_ANY, &target, SDP_RETRY_IF_BUSY);
    if (!session) {
        log_msg("ℹ️ SDP bağlantısı kurulamadı, varsayılan kanal kullanılacak");
        return 0;
    }
    
    // HFP-AG UUID (0x111F)
    uuid_t hfp_ag_uuid;
    sdp_uuid16_create(&hfp_ag_uuid, 0x111F);
    
    // Arama listesi
    sdp_list_t *search_list = sdp_list_append(NULL, &hfp_ag_uuid);
    
    // İstenen özellikler
    uint32_t range = 0x0000ffff;
    sdp_list_t *attrid_list = sdp_list_append(NULL, &range);
    
    // SDP sorgusu yap
    sdp_list_t *response_list = NULL;
    int err = sdp_service_search_attr_req(session, search_list, SDP_ATTR_REQ_RANGE, attrid_list, &response_list);
    
    sdp_list_free(search_list, NULL);
    sdp_list_free(attrid_list, NULL);
    
    uint8_t channel = 0;
    
    if (err == 0 && response_list) {
        sdp_list_t *r = response_list;
        for (; r; r = r->next) {
            sdp_record_t *rec = (sdp_record_t *)r->data;
            sdp_list_t *proto_list = NULL;
            
            if (sdp_get_access_protos(rec, &proto_list) == 0) {
                sdp_list_t *p = proto_list;
                for (; p; p = p->next) {
                    sdp_list_t *pds = (sdp_list_t *)p->data;
                    for (; pds; pds = pds->next) {
                        sdp_data_t *d = (sdp_data_t *)pds->data;
                        int proto = 0;
                        for (; d; d = d->next) {
                            switch (d->dtd) {
                                case SDP_UUID16:
                                case SDP_UUID32:
                                case SDP_UUID128:
                                    proto = sdp_uuid_to_proto(&d->val.uuid);
                                    break;
                                case SDP_UINT8:
                                    if (proto == RFCOMM_UUID) {
                                        channel = d->val.uint8;
                                    }
                                    break;
                            }
                        }
                    }
                    sdp_list_free((sdp_list_t *)p->data, NULL);
                }
                sdp_list_free(proto_list, NULL);
            }
            sdp_record_free(rec);
            if (channel) break;  // Bulundu
        }
        sdp_list_free(response_list, NULL);
    }
    
    sdp_close(session);
    
    if (channel) {
        char msg[64];
        snprintf(msg, sizeof(msg), "✓ HFP-AG kanalı bulundu: %d", channel);
        log_msg(msg);
    }
    
    return channel;
}

// ============================================================================
// CSV DATABASE
// ============================================================================

static void save_contacts_to_csv(void) {
    FILE *f = fopen(CONTACTS_CSV, "w");
    if (!f) return;
    fprintf(f, "name,number\n");
    for (int i = 0; i < all_contacts_count; i++) {
        // CSV escape - virgül ve tırnak için
        fprintf(f, "\"%s\",\"%s\"\n", all_contacts[i].name, all_contacts[i].number);
    }
    fclose(f);
}

static gboolean load_contacts_from_csv(void) {
    FILE *f = fopen(CONTACTS_CSV, "r");
    if (!f) return FALSE;
    
    char line[512];
    all_contacts_count = 0;
    
    // Header'ı atla
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return FALSE;
    }
    
    while (fgets(line, sizeof(line), f) && all_contacts_count < 2000) {
        // Format: "name","number"
        char *p = line;
        if (*p == '"') p++;
        
        char *name_end = strstr(p, "\",\"");
        if (!name_end) continue;
        *name_end = '\0';
        strncpy(all_contacts[all_contacts_count].name, p, 127);
        
        p = name_end + 3;
        char *num_end = strchr(p, '"');
        if (num_end) *num_end = '\0';
        char *nl = strchr(p, '\n'); if (nl) *nl = '\0';
        strncpy(all_contacts[all_contacts_count].number, p, 63);
        
        all_contacts_count++;
    }
    fclose(f);
    return (all_contacts_count > 0);
}

static void save_recents_to_csv(void) {
    FILE *f = fopen(RECENTS_CSV, "w");
    if (!f) return;
    fprintf(f, "type,name,number,time\n");
    for (int i = 0; i < recent_count; i++) {
        fprintf(f, "\"%s\",\"%s\",\"%s\",\"%s\"\n", 
            recent_entries[i].type, recent_entries[i].name, 
            recent_entries[i].number, recent_entries[i].time);
    }
    fclose(f);
}

static gboolean load_recents_from_csv(void) {
    FILE *f = fopen(RECENTS_CSV, "r");
    if (!f) return FALSE;
    
    char line[512];
    recent_count = 0;
    
    // Header'ı atla
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return FALSE;
    }
    
    while (fgets(line, sizeof(line), f) && recent_count < 500) {
        // Format: "type","name","number","time"
        char *fields[4] = {NULL};
        char *p = line;
        
        for (int i = 0; i < 4 && p; i++) {
            if (*p == '"') p++;
            fields[i] = p;
            char *end = strstr(p, "\",\"");
            if (end) {
                *end = '\0';
                p = end + 3;
            } else {
                // Son alan
                char *quote = strchr(p, '"');
                if (quote) *quote = '\0';
                char *nl = strchr(p, '\n'); if (nl) *nl = '\0';
                p = NULL;
            }
        }
        
        if (fields[0] && fields[1] && fields[2] && fields[3]) {
            strncpy(recent_entries[recent_count].type, fields[0], 15);
            strncpy(recent_entries[recent_count].name, fields[1], 127);
            strncpy(recent_entries[recent_count].number, fields[2], 63);
            strncpy(recent_entries[recent_count].time, fields[3], 63);
            recent_count++;
        }
    }
    fclose(f);
    return (recent_count > 0);
}

// ============================================================================
// LOG
// ============================================================================

static gboolean append_log_ui(gpointer data) {
    char *full_msg = (char *)data;
    if (log_buffer) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(log_buffer, &end);
        gtk_text_buffer_insert(log_buffer, &end, full_msg, -1);

        GtkTextMark *mark = gtk_text_buffer_get_insert(log_buffer);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(log_view), mark, 0.0, TRUE, 0.0, 1.0);
    }
    g_free(full_msg);
    return G_SOURCE_REMOVE;
}

static void log_msg(const char *msg) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "[%H:%M:%S]", t);

    char full_msg[512];
    snprintf(full_msg, sizeof(full_msg), "%s %s\n", time_str, msg);
    printf("%s", full_msg);

    if (log_buffer) {
        g_idle_add(append_log_ui, g_strdup(full_msg));
    }
}

// ============================================================================
// WINDOW + RINGTONE
// ============================================================================

static gboolean disable_keep_above(gpointer data) {
    (void)data;
    if (window && current_call_state == CALL_IDLE) {
        gtk_window_set_keep_above(GTK_WINDOW(window), FALSE);
        gtk_window_set_urgency_hint(GTK_WINDOW(window), FALSE);
    }
    return G_SOURCE_REMOVE;
}

static void bring_window_to_front(void) {
    if (!window) return;
    
    // Pencereyi görünür yap
    gtk_widget_show(window);
    
    // Minimize edilmişse geri getir
    gtk_window_deiconify(GTK_WINDOW(window));
    
    // Her zaman üstünde tut (geçici)
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    
    // Acil durum ipucu
    gtk_window_set_urgency_hint(GTK_WINDOW(window), TRUE);
    
    // Pencereyi öne getir
    gtk_window_present_with_time(GTK_WINDOW(window), GDK_CURRENT_TIME);
    
    // Ses çal
    gdk_display_beep(gdk_display_get_default());
}

static gboolean ringtone_tick(gpointer data) {
    (void)data;
    gdk_display_beep(gdk_display_get_default());
    return G_SOURCE_CONTINUE;
}

static void start_ringtone(void) {
    if (ringtone_timer_id != 0) return;
    ringtone_timer_id = g_timeout_add(1000, ringtone_tick, NULL);
}

static void stop_ringtone(void) {
    if (ringtone_timer_id == 0) return;
    g_source_remove(ringtone_timer_id);
    ringtone_timer_id = 0;
}

// ============================================================================
// CONTACTS
// ============================================================================

static void add_contact(const char *name, const char *number) {
    if (contacts_count >= (int)(sizeof(contacts) / sizeof(contacts[0]))) return;
    strncpy(contacts[contacts_count].name, name, sizeof(contacts[contacts_count].name) - 1);
    strncpy(contacts[contacts_count].number, number, sizeof(contacts[contacts_count].number) - 1);
    contacts_count++;
}

static void load_contacts_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        log_msg("ℹ️ contacts.csv bulunamadı, kişi listesi boş");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *comma = strchr(line, ',');
        if (!comma) continue;
        *comma = '\0';

        char *name = line;
        char *number = comma + 1;

        // Trim newline
        char *nl = strchr(number, '\n');
        if (nl) *nl = '\0';

        if (name[0] && number[0]) {
            add_contact(name, number);
        }
    }
    fclose(f);
}

static const char *lookup_contact_name(const char *number) {
    for (int i = 0; i < contacts_count; i++) {
        if (strcmp(contacts[i].number, number) == 0) return contacts[i].name;
    }
    return NULL;
}

// ============================================================================
// SETTINGS JSON
// ============================================================================

static void load_settings(void) {
    FILE *f = fopen(SETTINGS_JSON, "r");
    if (!f) return;
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int val;
        if (sscanf(line, " \"col_recent_type\" : %d", &val) == 1) col_recent_type = val;
        else if (sscanf(line, " \"col_recent_name\" : %d", &val) == 1) col_recent_name = val;
        else if (sscanf(line, " \"col_recent_number\" : %d", &val) == 1) col_recent_number = val;
        else if (sscanf(line, " \"col_recent_time\" : %d", &val) == 1) col_recent_time = val;
        else if (sscanf(line, " \"col_contacts_name\" : %d", &val) == 1) col_contacts_name = val;
        else if (sscanf(line, " \"col_contacts_number\" : %d", &val) == 1) col_contacts_number = val;
    }
    fclose(f);
}

static void save_settings(void) {
    FILE *f = fopen(SETTINGS_JSON, "w");
    if (!f) return;
    
    fprintf(f, "{\n");
    fprintf(f, "  \"col_recent_type\": %d,\n", col_recent_type);
    fprintf(f, "  \"col_recent_name\": %d,\n", col_recent_name);
    fprintf(f, "  \"col_recent_number\": %d,\n", col_recent_number);
    fprintf(f, "  \"col_recent_time\": %d,\n", col_recent_time);
    fprintf(f, "  \"col_contacts_name\": %d,\n", col_contacts_name);
    fprintf(f, "  \"col_contacts_number\": %d\n", col_contacts_number);
    fprintf(f, "}\n");
    fclose(f);
}

// GTK destroy callback wrapper
static void on_window_destroy(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    save_settings();
}

// Sütun genişlik değişim callback'leri
static void on_col_recent_type_width(GObject *obj, GParamSpec *pspec, gpointer data) {
    (void)pspec; (void)data;
    col_recent_type = gtk_tree_view_column_get_width(GTK_TREE_VIEW_COLUMN(obj));
}
static void on_col_recent_name_width(GObject *obj, GParamSpec *pspec, gpointer data) {
    (void)pspec; (void)data;
    col_recent_name = gtk_tree_view_column_get_width(GTK_TREE_VIEW_COLUMN(obj));
}
static void on_col_recent_number_width(GObject *obj, GParamSpec *pspec, gpointer data) {
    (void)pspec; (void)data;
    col_recent_number = gtk_tree_view_column_get_width(GTK_TREE_VIEW_COLUMN(obj));
}
static void on_col_recent_time_width(GObject *obj, GParamSpec *pspec, gpointer data) {
    (void)pspec; (void)data;
    col_recent_time = gtk_tree_view_column_get_width(GTK_TREE_VIEW_COLUMN(obj));
}
static void on_col_contacts_name_width(GObject *obj, GParamSpec *pspec, gpointer data) {
    (void)pspec; (void)data;
    col_contacts_name = gtk_tree_view_column_get_width(GTK_TREE_VIEW_COLUMN(obj));
}
static void on_col_contacts_number_width(GObject *obj, GParamSpec *pspec, gpointer data) {
    (void)pspec; (void)data;
    col_contacts_number = gtk_tree_view_column_get_width(GTK_TREE_VIEW_COLUMN(obj));
}

static void refresh_contacts_view(void) {
    if (!contacts_store) return;
    gtk_list_store_clear(contacts_store);
    for (int i = 0; i < contacts_count; i++) {
        GtkTreeIter iter;
        gtk_list_store_append(contacts_store, &iter);
        gtk_list_store_set(contacts_store, &iter,
                          0, contacts[i].name,
                          1, contacts[i].number,
                          -1);
    }
}

static void refresh_recents_view(void) {
    if (!recent_store) return;
    gtk_list_store_clear(recent_store);
    for (int i = 0; i < recent_count; i++) {
        GtkTreeIter iter;
        gtk_list_store_append(recent_store, &iter);
        gtk_list_store_set(recent_store, &iter,
                          0, recent_entries[i].type,
                          1, recent_entries[i].name,
                          2, recent_entries[i].number,
                          3, recent_entries[i].time,
                          -1);
    }
}

// ============================================================================
// PBAP (Phonebook Access) via obexd
// ============================================================================

static gboolean ensure_obexd_running(void) {
    if (obex_conn) return TRUE;

    log_msg("ℹ️ obexd servisi başlatılıyor...");

    GError *error = NULL;
    gchar *addr = NULL;
    const gchar *sudo_user = NULL;
    const gchar *sudo_uid = NULL;

    // Önce mevcut session bus'ı kontrol et
    const gchar *env_addr = g_getenv("DBUS_SESSION_BUS_ADDRESS");
    if (env_addr && *env_addr) {
        addr = g_strdup(env_addr);
        log_msg("ℹ️ Mevcut session bus kullanılıyor");
    } else {
        // Kullanıcının session bus'ını bul (sudo ile çalışırken)
        uid_t real_uid = getuid();
        if (real_uid == 0) {
            sudo_user = g_getenv("SUDO_USER");
            sudo_uid = g_getenv("SUDO_UID");
            if (sudo_uid) {
                gchar *bus_path = g_strdup_printf("/run/user/%s/bus", sudo_uid);
                if (g_file_test(bus_path, G_FILE_TEST_EXISTS)) {
                    addr = g_strdup_printf("unix:path=%s", bus_path);
                    log_msg("ℹ️ Kullanıcı session bus'ı bulundu");
                }
                g_free(bus_path);
            }
        }
    }

    if (!addr || !*addr) {
        log_msg("⚠️ Session bus bulunamadı");
        g_free(addr);
        return FALSE;
    }

    // Önce mevcut obexd'ye bağlanmayı dene
    error = NULL;
    obex_conn = g_dbus_connection_new_for_address_sync(
        addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);

    if (obex_conn) {
        // obexd zaten çalışıyor mu kontrol et
        GVariant *result = g_dbus_connection_call_sync(
            obex_conn,
            "org.bluez.obex",
            "/org/bluez/obex",
            "org.freedesktop.DBus.Introspectable",
            "Introspect",
            NULL,
            G_VARIANT_TYPE("(s)"),
            G_DBUS_CALL_FLAGS_NONE,
            1000,
            NULL,
            NULL);

        if (result) {
            g_variant_unref(result);
            log_msg("✓ Mevcut obexd servisi kullanılıyor");
            g_free(addr);
            return TRUE;
        }
        g_object_unref(obex_conn);
        obex_conn = NULL;
    }
    if (error) {
        g_error_free(error);
        error = NULL;
    }

    // obexd çalışmıyor, kullanıcı olarak başlat
    gboolean started = FALSE;
    if (sudo_user && *sudo_user) {
        // sudo -u <user> ile obexd başlat
        gchar *cmd = g_strdup_printf(
            "sudo -u %s DBUS_SESSION_BUS_ADDRESS='%s' /usr/libexec/bluetooth/obexd -n &",
            sudo_user, addr);
        log_msg("ℹ️ obexd kullanıcı olarak başlatılıyor...");
        int ret = system(cmd);
        g_free(cmd);
        if (ret == 0) {
            started = TRUE;
            usleep(500000); // 500ms bekle
        }
    } else {
        // Normal kullanıcı olarak çalışıyoruz
        const char *candidates[] = {
            "/usr/libexec/bluetooth/obexd",
            "/usr/lib/bluetooth/obexd",
            "obexd",
            NULL
        };

        for (int i = 0; candidates[i]; i++) {
            gchar *argv[] = { (gchar *)candidates[i], (gchar *)"-n", NULL };
            gchar **envp = g_get_environ();
            envp = g_environ_setenv(envp, "DBUS_SESSION_BUS_ADDRESS", addr, TRUE);
            if (g_spawn_async(NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
                started = TRUE;
                g_strfreev(envp);
                usleep(500000);
                break;
            }
            g_strfreev(envp);
            if (error) {
                g_error_free(error);
                error = NULL;
            }
        }
    }

    if (!started) {
        log_msg("⚠️ obexd başlatılamadı");
        g_free(addr);
        return FALSE;
    }

    // obexd'nin başlamasını bekle ve session bus'a bağlan
    log_msg("ℹ️ obexd başlatıldı, bağlantı kuruluyor...");
    
    for (int retry = 0; retry < 10; retry++) {
        usleep(300000); // 300ms bekle
        
        error = NULL;
        obex_conn = g_dbus_connection_new_for_address_sync(
            addr,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL,
            NULL,
            &error);

        if (!obex_conn) {
            if (error) {
                g_error_free(error);
                error = NULL;
            }
            continue;
        }

        // obexd servisinin hazır olup olmadığını kontrol et
        GVariant *result = g_dbus_connection_call_sync(
            obex_conn,
            "org.bluez.obex",
            "/org/bluez/obex",
            "org.freedesktop.DBus.Introspectable",
            "Introspect",
            NULL,
            G_VARIANT_TYPE("(s)"),
            G_DBUS_CALL_FLAGS_NONE,
            1000,
            NULL,
            NULL);

        if (result) {
            g_variant_unref(result);
            log_msg("✓ obexd bağlantısı kuruldu");
            g_free(addr);
            return TRUE;
        }

        g_object_unref(obex_conn);
        obex_conn = NULL;
    }

    g_free(addr);
    log_msg("⚠️ obexd bağlantısı kurulamadı (timeout)");
    return FALSE;
}

static void parse_vcf_contacts(const char *file_path) {
    FILE *f = fopen(file_path, "r");
    if (!f) {
        log_msg("⚠️ VCF dosyası açılamadı");
        return;
    }

    contacts_count = 0;
    char line[512];
    char name[128] = {0};
    char number[64] = {0};

    while (fgets(line, sizeof(line), f)) {
        if (g_str_has_prefix(line, "FN:")) {
            strncpy(name, line + 3, sizeof(name) - 1);
            char *nl = strchr(name, '\n'); if (nl) *nl = '\0';
            char *cr = strchr(name, '\r'); if (cr) *cr = '\0';
        } else if (g_str_has_prefix(line, "TEL")) {
            char *colon = strchr(line, ':');
            if (colon) {
                strncpy(number, colon + 1, sizeof(number) - 1);
                char *nl = strchr(number, '\n'); if (nl) *nl = '\0';
                char *cr = strchr(number, '\r'); if (cr) *cr = '\0';
            }
        } else if (g_str_has_prefix(line, "END:VCARD")) {
            if (number[0]) {
                if (!name[0]) strncpy(name, number, sizeof(name) - 1);
                add_contact(name, number);
            }
            memset(name, 0, sizeof(name));
            memset(number, 0, sizeof(number));
        }
    }

    fclose(f);
}

static void parse_vcf_recents(const char *file_path, const char *type_label) {
    FILE *f = fopen(file_path, "r");
    if (!f) {
        return;
    }

    char line[512];
    char name[128] = {0};
    char number[64] = {0};
    char datetime[64] = {0};
    char raw_datetime[20] = {0};

    while (fgets(line, sizeof(line), f)) {
        if (g_str_has_prefix(line, "FN:")) {
            strncpy(name, line + 3, sizeof(name) - 1);
            char *nl = strchr(name, '\n'); if (nl) *nl = '\0';
            char *cr = strchr(name, '\r'); if (cr) *cr = '\0';
        } else if (g_str_has_prefix(line, "TEL")) {
            char *colon = strchr(line, ':');
            if (colon) {
                strncpy(number, colon + 1, sizeof(number) - 1);
                char *nl = strchr(number, '\n'); if (nl) *nl = '\0';
                char *cr = strchr(number, '\r'); if (cr) *cr = '\0';
            }
        } else if (g_str_has_prefix(line, "X-IRMC-CALL-DATETIME")) {
            // Format: X-IRMC-CALL-DATETIME;RECEIVED:20260120T031500 veya sadece :20260120T031500
            char *value = strchr(line, ':');
            if (value) {
                value++;  // ':' karakterini atla
                // Zaman damgasını formatla: 20260120T031500 -> 20.01.2026 03:15
                char raw[32] = {0};
                strncpy(raw, value, sizeof(raw) - 1);
                char *nl = strchr(raw, '\n'); if (nl) *nl = '\0';
                char *cr = strchr(raw, '\r'); if (cr) *cr = '\0';
                
                // Raw formatı sakla (sıralama için)
                strncpy(raw_datetime, raw, sizeof(raw_datetime) - 1);
                
                if (strlen(raw) >= 15) {  // 20260120T031500
                    char year[5] = {0}, month[3] = {0}, day[3] = {0};
                    char hour[3] = {0}, min[3] = {0};
                    strncpy(year, raw, 4);
                    strncpy(month, raw + 4, 2);
                    strncpy(day, raw + 6, 2);
                    strncpy(hour, raw + 9, 2);
                    strncpy(min, raw + 11, 2);
                    snprintf(datetime, sizeof(datetime), "%s.%s.%s %s:%s", day, month, year, hour, min);
                } else if (strlen(raw) > 0) {
                    strncpy(datetime, raw, sizeof(datetime) - 1);
                }
            }
        } else if (g_str_has_prefix(line, "END:VCARD")) {
            if (number[0]) {
                if (recent_count < (int)(sizeof(recent_entries) / sizeof(recent_entries[0]))) {
                    RecentEntry *entry = &recent_entries[recent_count++];
                    strncpy(entry->type, type_label, sizeof(entry->type) - 1);
                    strncpy(entry->name, name[0] ? name : "-", sizeof(entry->name) - 1);
                    strncpy(entry->number, number, sizeof(entry->number) - 1);
                    strncpy(entry->time, datetime[0] ? datetime : "-", sizeof(entry->time) - 1);
                    strncpy(entry->raw_time, raw_datetime, sizeof(entry->raw_time) - 1);
                }
            }
            memset(name, 0, sizeof(name));
            memset(number, 0, sizeof(number));
            memset(datetime, 0, sizeof(datetime));
            memset(raw_datetime, 0, sizeof(raw_datetime));
        }
    }

    fclose(f);
}

static gboolean contacts_sync_start_cb(gpointer data) {
    (void)data;
    syncing_contacts = TRUE;
    if (contacts_spinner) {
        gtk_spinner_start(GTK_SPINNER(contacts_spinner));
        gtk_widget_show(contacts_spinner);
    }
    if (contacts_search_entry) {
        gtk_widget_set_sensitive(contacts_search_entry, FALSE);
    }
    return G_SOURCE_REMOVE;
}

static gboolean contacts_sync_complete_cb(gpointer data) {
    gboolean success = GPOINTER_TO_INT(data);
    syncing_contacts = FALSE;
    if (contacts_spinner) {
        gtk_spinner_stop(GTK_SPINNER(contacts_spinner));
        gtk_widget_hide(contacts_spinner);
    }
    if (success) {
        refresh_contacts_view();
        log_msg("✓ Rehber güncellendi");
    } else {
        log_msg("⚠️ Rehber alınamadı");
    }
    update_ui();
    return G_SOURCE_REMOVE;
}

static gpointer sync_contacts_thread(gpointer data) {
    (void)data;
    
    g_idle_add(contacts_sync_start_cb, NULL);
    
    if (!device_addr[0]) {
        log_msg("⚠️ Telefon adresi yok");
        g_idle_add(contacts_sync_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }

    if (!ensure_obexd_running()) {
        log_msg("⚠️ obexd bulunamadı");
        g_idle_add(contacts_sync_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }

    // Session oluştur
    GError *error = NULL;
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "Target", g_variant_new_string("PBAP"));

    GVariant *result = g_dbus_connection_call_sync(
        obex_conn, "org.bluez.obex", "/org/bluez/obex",
        "org.bluez.obex.Client1", "CreateSession",
        g_variant_new("(sa{sv})", device_addr, &opts),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);

    if (error || !result) {
        if (error) {
            snprintf(error_msg, sizeof(error_msg), "PBAP session hatası: %s", error->message);
            log_msg(error_msg);
            g_error_free(error);
        }
        g_idle_add(contacts_sync_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }

    const gchar *session_path = NULL;
    g_variant_get(result, "(&o)", &session_path);
    if (!session_path) {
        g_variant_unref(result);
        g_idle_add(contacts_sync_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }
    
    gchar *session_copy = g_strdup(session_path);
    g_variant_unref(result);
    
    // PhonebookAccess1 hazır olana kadar bekle
    log_msg("ℹ️ Telefonda rehber izni bekleniyor...");
    gboolean ready = FALSE;
    for (int i = 0; i < 30; i++) {
        g_usleep(1000 * 1000);
        GVariant *intr = g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", session_copy,
            "org.freedesktop.DBus.Introspectable", "Introspect",
            NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        if (intr) {
            const gchar *xml = NULL;
            g_variant_get(intr, "(&s)", &xml);
            if (xml && strstr(xml, "PhonebookAccess1")) ready = TRUE;
            g_variant_unref(intr);
            if (ready) break;
        }
    }
    
    if (!ready) {
        log_msg("⚠️ Telefonda rehber izni verilmedi");
        g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", "/org/bluez/obex",
            "org.bluez.obex.Client1", "RemoveSession",
            g_variant_new("(o)", session_copy), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        g_free(session_copy);
        g_idle_add(contacts_sync_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }
    
    log_msg("✓ Rehber erişimi hazır");
    
    // Select phonebook
    error = NULL;
    g_dbus_connection_call_sync(
        obex_conn, "org.bluez.obex", session_copy,
        "org.bluez.obex.PhonebookAccess1", "Select",
        g_variant_new("(ss)", "int", "pb"),
        NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
    
    if (error) {
        g_error_free(error);
        error = NULL;
        g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", session_copy,
            "org.bluez.obex.PhonebookAccess1", "Select",
            g_variant_new("(ss)", "", "pb"),
            NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
        if (error) {
            snprintf(error_msg, sizeof(error_msg), "PBAP Select hatası: %s", error->message);
            log_msg(error_msg);
            g_error_free(error);
            g_dbus_connection_call_sync(
                obex_conn, "org.bluez.obex", "/org/bluez/obex",
                "org.bluez.obex.Client1", "RemoveSession",
                g_variant_new("(o)", session_copy), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
            g_free(session_copy);
            g_idle_add(contacts_sync_complete_cb, GINT_TO_POINTER(FALSE));
            return NULL;
        }
    }
    
    // PullAll
    GVariantBuilder pull_opts;
    g_variant_builder_init(&pull_opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&pull_opts, "{sv}", "Format", g_variant_new_string("vcard21"));
    
    error = NULL;
    result = g_dbus_connection_call_sync(
        obex_conn, "org.bluez.obex", session_copy,
        "org.bluez.obex.PhonebookAccess1", "PullAll",
        g_variant_new("(sa{sv})", "", &pull_opts),
        G_VARIANT_TYPE("(oa{sv})"), G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);
    
    if (error || !result) {
        if (error) {
            snprintf(error_msg, sizeof(error_msg), "PBAP PullAll hatası: %s", error->message);
            log_msg(error_msg);
            g_error_free(error);
        }
        g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", "/org/bluez/obex",
            "org.bluez.obex.Client1", "RemoveSession",
            g_variant_new("(o)", session_copy), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        g_free(session_copy);
        g_idle_add(contacts_sync_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }
    
    const gchar *transfer_path = NULL;
    GVariant *props = NULL;
    g_variant_get(result, "(&o@a{sv})", &transfer_path, &props);
    
    gchar *filename = NULL;
    if (props) {
        GVariant *fname_var = g_variant_lookup_value(props, "Filename", G_VARIANT_TYPE_STRING);
        if (fname_var) {
            filename = g_strdup(g_variant_get_string(fname_var, NULL));
            g_variant_unref(fname_var);
        }
        g_variant_unref(props);
    }
    
    if (transfer_path) {
        gchar *tpath = g_strdup(transfer_path);
        g_variant_unref(result);
        
        log_msg("ℹ️ Rehber indiriliyor...");
        
        // Transfer tamamlanana kadar bekle
        for (int i = 0; i < 100; i++) {
            g_usleep(100 * 1000);
            GVariant *status_var = g_dbus_connection_call_sync(
                obex_conn, "org.bluez.obex", tpath,
                "org.freedesktop.DBus.Properties", "Get",
                g_variant_new("(ss)", "org.bluez.obex.Transfer1", "Status"),
                G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
            
            if (status_var) {
                GVariant *inner;
                g_variant_get(status_var, "(v)", &inner);
                const gchar *status = g_variant_get_string(inner, NULL);
                gboolean complete = (status && g_strcmp0(status, "complete") == 0);
                g_variant_unref(inner);
                g_variant_unref(status_var);
                
                if (complete) {
                    if (!filename) {
                        GVariant *file_var = g_dbus_connection_call_sync(
                            obex_conn, "org.bluez.obex", tpath,
                            "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new("(ss)", "org.bluez.obex.Transfer1", "Filename"),
                            G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
                        if (file_var) {
                            GVariant *inner_file;
                            g_variant_get(file_var, "(v)", &inner_file);
                            filename = g_strdup(g_variant_get_string(inner_file, NULL));
                            g_variant_unref(inner_file);
                            g_variant_unref(file_var);
                        }
                    }
                    break;
                }
            }
        }
        g_free(tpath);
    } else {
        g_variant_unref(result);
    }
    
    gboolean success = FALSE;
    if (filename) {
        parse_vcf_contacts(filename);
        g_free(filename);
        success = (contacts_count > 0);
    }
    
    // Session'ı kapat
    if (session_copy && session_copy[0]) {
        g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", "/org/bluez/obex",
            "org.bluez.obex.Client1", "RemoveSession",
            g_variant_new("(o)", session_copy), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    }
    g_free(session_copy);
    g_idle_add(contacts_sync_complete_cb, GINT_TO_POINTER(success));
    return NULL;
}

// Arama sonuçlarını güncelle (UI thread)
static gboolean search_results_update_cb(gpointer data) {
    (void)data;
    syncing_contacts = FALSE;
    if (contacts_spinner) {
        gtk_spinner_stop(GTK_SPINNER(contacts_spinner));
        gtk_widget_hide(contacts_spinner);
    }
    refresh_contacts_view();
    return G_SOURCE_REMOVE;
}

// Arama thread'i - sadece hafızadan arar (çok hızlı)
static gpointer search_contacts_thread(gpointer data) {
    gchar *query = (gchar *)data;
    
    contacts_count = 0;
    
    if (!query || strlen(query) < 2) {
        g_free(query);
        g_idle_add(search_results_update_cb, NULL);
        return NULL;
    }
    
    // Rehber yüklü değilse mesaj ver
    if (!phonebook_loaded || all_contacts_count == 0) {
        g_free(query);
        g_idle_add(search_results_update_cb, NULL);
        return NULL;
    }
    
    // Hafızadaki rehberden ara
    gchar *query_lower = g_utf8_strdown(query, -1);
    
    for (int i = 0; i < all_contacts_count && contacts_count < 200; i++) {
        gchar *name_lower = g_utf8_strdown(all_contacts[i].name, -1);
        if (strstr(name_lower, query_lower) || strstr(all_contacts[i].number, query)) {
            strncpy(contacts[contacts_count].name, all_contacts[i].name, sizeof(contacts[0].name) - 1);
            strncpy(contacts[contacts_count].number, all_contacts[i].number, sizeof(contacts[0].number) - 1);
            contacts_count++;
        }
        g_free(name_lower);
    }
    
    g_free(query_lower);
    g_free(query);
    g_idle_add(search_results_update_cb, NULL);
    return NULL;
}

// Rehber yüklendiğinde UI güncelle
static gboolean phonebook_load_complete_cb(gpointer data) {
    gboolean success = GPOINTER_TO_INT(data);
    syncing_contacts = FALSE;
    if (contacts_spinner) {
        gtk_spinner_stop(GTK_SPINNER(contacts_spinner));
        gtk_widget_hide(contacts_spinner);
    }
    if (success) {
        char msg[64];
        snprintf(msg, sizeof(msg), "✓ Rehber yüklendi: %d kişi", all_contacts_count);
        log_msg(msg);
    }
    // Bekleyen arama varsa çalıştır
    if (pending_search_query && strlen(pending_search_query) >= 2) {
        g_thread_new("search_contacts", search_contacts_thread, g_strdup(pending_search_query));
    }
    
    // Rehber yüklendikten sonra son aramaları yükle
    if (current_state == STATE_CONNECTED && !syncing_recents && recent_count == 0) {
        g_thread_new("load_recents", sync_recents_thread, NULL);
    }
    
    return G_SOURCE_REMOVE;
}

// Rehberi PBAP'tan yükle (arka plan thread)
static gpointer load_phonebook_thread(gpointer data) {
    (void)data;
    
    if (!device_addr[0] || !ensure_obexd_running()) {
        g_idle_add(phonebook_load_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }
    
    // PBAP session oluştur
    GError *error = NULL;
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "Target", g_variant_new_string("PBAP"));
    
    GVariant *result = g_dbus_connection_call_sync(
        obex_conn, "org.bluez.obex", "/org/bluez/obex",
        "org.bluez.obex.Client1", "CreateSession",
        g_variant_new("(sa{sv})", device_addr, &opts),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 15000, NULL, &error);
    
    if (error || !result) {
        if (error) g_error_free(error);
        g_idle_add(phonebook_load_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }
    
    const gchar *session_path = NULL;
    g_variant_get(result, "(&o)", &session_path);
    gchar *session_copy = g_strdup(session_path);
    g_variant_unref(result);
    
    // PhonebookAccess1 hazır olana kadar bekle
    gboolean ready = FALSE;
    for (int i = 0; i < 15; i++) {
        g_usleep(100 * 1000);
        GVariant *intr = g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", session_copy,
            "org.freedesktop.DBus.Introspectable", "Introspect",
            NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        if (intr) {
            const gchar *xml = NULL;
            g_variant_get(intr, "(&s)", &xml);
            if (xml && strstr(xml, "PhonebookAccess1")) ready = TRUE;
            g_variant_unref(intr);
            if (ready) break;
        }
    }
    
    if (!ready) {
        g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", "/org/bluez/obex",
            "org.bluez.obex.Client1", "RemoveSession",
            g_variant_new("(o)", session_copy), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        g_free(session_copy);
        g_idle_add(phonebook_load_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }
    
    // Select phonebook
    g_dbus_connection_call_sync(
        obex_conn, "org.bluez.obex", session_copy,
        "org.bluez.obex.PhonebookAccess1", "Select",
        g_variant_new("(ss)", "int", "pb"), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    
    // PullAll - tüm rehberi çek
    GVariantBuilder pull_opts;
    g_variant_builder_init(&pull_opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&pull_opts, "{sv}", "Format", g_variant_new_string("vcard21"));
    
    result = g_dbus_connection_call_sync(
        obex_conn, "org.bluez.obex", session_copy,
        "org.bluez.obex.PhonebookAccess1", "PullAll",
        g_variant_new("(sa{sv})", "", &pull_opts),
        G_VARIANT_TYPE("(oa{sv})"), G_DBUS_CALL_FLAGS_NONE, 60000, NULL, NULL);
    
    gboolean success = FALSE;
    
    if (result) {
        const gchar *transfer_path = NULL;
        GVariant *props = NULL;
        g_variant_get(result, "(&o@a{sv})", &transfer_path, &props);
        
        gchar *filename = NULL;
        if (props) {
            GVariant *fname_var = g_variant_lookup_value(props, "Filename", G_VARIANT_TYPE_STRING);
            if (fname_var) {
                filename = g_strdup(g_variant_get_string(fname_var, NULL));
                g_variant_unref(fname_var);
            }
            g_variant_unref(props);
        }
        
        if (transfer_path) {
            gchar *tpath = g_strdup(transfer_path);
            g_variant_unref(result);
            
            // 30 saniye bekle (300 x 100ms) - büyük rehberler için
            for (int i = 0; i < 300; i++) {
                g_usleep(100 * 1000);
                GVariant *status_var = g_dbus_connection_call_sync(
                    obex_conn, "org.bluez.obex", tpath,
                    "org.freedesktop.DBus.Properties", "Get",
                    g_variant_new("(ss)", "org.bluez.obex.Transfer1", "Status"),
                    G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
                
                if (status_var) {
                    GVariant *inner;
                    g_variant_get(status_var, "(v)", &inner);
                    const gchar *status = g_variant_get_string(inner, NULL);
                    gboolean complete = (status && g_strcmp0(status, "complete") == 0);
                    g_variant_unref(inner);
                    g_variant_unref(status_var);
                    
                    if (complete) {
                        if (!filename) {
                            GVariant *file_var = g_dbus_connection_call_sync(
                                obex_conn, "org.bluez.obex", tpath,
                                "org.freedesktop.DBus.Properties", "Get",
                                g_variant_new("(ss)", "org.bluez.obex.Transfer1", "Filename"),
                                G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
                            if (file_var) {
                                GVariant *inner_file;
                                g_variant_get(file_var, "(v)", &inner_file);
                                filename = g_strdup(g_variant_get_string(inner_file, NULL));
                                g_variant_unref(inner_file);
                                g_variant_unref(file_var);
                            }
                        }
                        break;
                    }
                }
            }
            g_free(tpath);
        } else {
            g_variant_unref(result);
        }
        
        if (filename) {
            // VCF parse et - tüm rehberi all_contacts'a yükle
            char logbuf[256];
            snprintf(logbuf, sizeof(logbuf), "Rehber dosyası: %s", filename);
            log_msg(logbuf);
            FILE *f = fopen(filename, "r");
            if (f) {
                all_contacts_count = 0;
                char line[512], name[128] = {0}, number[64] = {0};
                int total_vcards = 0;
                
                while (fgets(line, sizeof(line), f)) {
                    if (g_str_has_prefix(line, "BEGIN:VCARD")) {
                        total_vcards++;
                    } else if (g_str_has_prefix(line, "FN:")) {
                        strncpy(name, line + 3, sizeof(name) - 1);
                        char *nl = strchr(name, '\n'); if (nl) *nl = '\0';
                        char *cr = strchr(name, '\r'); if (cr) *cr = '\0';
                    } else if (g_str_has_prefix(line, "N:") && !name[0]) {
                        // N: formatı: Soyad;Ad;Diğer...
                        char *start = line + 2;
                        char *semi = strchr(start, ';');
                        if (semi && semi[1]) {
                            char *name_end = strchr(semi + 1, ';');
                            if (name_end) *name_end = '\0';
                            char *nl = strchr(semi + 1, '\n'); if (nl) *nl = '\0';
                            char *cr = strchr(semi + 1, '\r'); if (cr) *cr = '\0';
                            snprintf(name, sizeof(name), "%s %s", semi + 1, start);
                            // Temizle
                            nl = strchr(name, ';'); if (nl) *nl = '\0';
                        }
                    } else if (g_str_has_prefix(line, "TEL")) {
                        char *colon = strchr(line, ':');
                        if (colon) {
                            strncpy(number, colon + 1, sizeof(number) - 1);
                            char *nl = strchr(number, '\n'); if (nl) *nl = '\0';
                            char *cr = strchr(number, '\r'); if (cr) *cr = '\0';
                        }
                    } else if (g_str_has_prefix(line, "END:VCARD")) {
                        if (name[0] && number[0] && all_contacts_count < 2000) {
                            strncpy(all_contacts[all_contacts_count].name, name, sizeof(all_contacts[0].name) - 1);
                            strncpy(all_contacts[all_contacts_count].number, number, sizeof(all_contacts[0].number) - 1);
                            all_contacts_count++;
                        }
                        name[0] = number[0] = '\0';
                    }
                }
                fclose(f);
                snprintf(logbuf, sizeof(logbuf), "VCF: %d kart, %d kişi yüklendi", total_vcards, all_contacts_count);
                log_msg(logbuf);
                phonebook_loaded = TRUE;
                save_contacts_to_csv();  // CSV'ye kaydet
                success = TRUE;
            }
            g_free(filename);
        }
    }
    
    // Session'ı kapat
    if (session_copy && session_copy[0]) {
        g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", "/org/bluez/obex",
            "org.bluez.obex.Client1", "RemoveSession",
            g_variant_new("(o)", session_copy), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    }
    g_free(session_copy);
    g_idle_add(phonebook_load_complete_cb, GINT_TO_POINTER(success));
    return NULL;
}

// Rehberi yenile butonu
static void on_refresh_phonebook_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    if (current_state != STATE_CONNECTED) {
        log_msg("⚠️ Rehber için telefon bağlı olmalı");
        return;
    }
    if (syncing_contacts) {
        log_msg("⚠️ Rehber zaten yükleniyor...");
        return;
    }
    phonebook_loaded = FALSE;
    all_contacts_count = 0;
    contacts_count = 0;
    refresh_contacts_view();
    
    syncing_contacts = TRUE;
    if (contacts_spinner) {
        gtk_spinner_start(GTK_SPINNER(contacts_spinner));
        gtk_widget_show(contacts_spinner);
    }
    log_msg("📥 Rehber yenileniyor...");
    g_thread_new("load_phonebook", load_phonebook_thread, NULL);
}

// Debounced arama - 500ms bekle
static gboolean do_search_timeout(gpointer data) {
    (void)data;
    search_timeout_id = 0;
    
    if (pending_search_query && strlen(pending_search_query) >= 2) {
        if (!syncing_contacts && current_state == STATE_CONNECTED) {
            // Rehber henüz yüklenmemişse önce yükle
            if (!phonebook_loaded) {
                syncing_contacts = TRUE;
                if (contacts_spinner) {
                    gtk_spinner_start(GTK_SPINNER(contacts_spinner));
                    gtk_widget_show(contacts_spinner);
                }
                log_msg("📥 Rehber ilk kez yükleniyor...");
                g_thread_new("load_phonebook", load_phonebook_thread, NULL);
            } else {
                // Rehber yüklü, hemen ara
                syncing_contacts = TRUE;
                if (contacts_spinner) {
                    gtk_spinner_start(GTK_SPINNER(contacts_spinner));
                    gtk_widget_show(contacts_spinner);
                }
                g_thread_new("search_contacts", search_contacts_thread, g_strdup(pending_search_query));
            }
        }
    } else {
        // Sorgu çok kısa, listeyi temizle
        contacts_count = 0;
        refresh_contacts_view();
    }
    
    return G_SOURCE_REMOVE;
}

// Arama kutusu değiştiğinde
static void on_contacts_search_changed(GtkSearchEntry *entry, gpointer data) {
    (void)data;
    
    g_free(pending_search_query);
    pending_search_query = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    
    // Önceki timeout'u iptal et
    if (search_timeout_id > 0) {
        g_source_remove(search_timeout_id);
    }
    
    // 500ms sonra ara
    search_timeout_id = g_timeout_add(500, do_search_timeout, NULL);
}

static gboolean recents_sync_start_cb(gpointer data) {
    (void)data;
    syncing_recents = TRUE;
    if (recents_spinner) {
        gtk_spinner_start(GTK_SPINNER(recents_spinner));
        gtk_widget_show(recents_spinner);
    }
    if (sync_recents_btn) {
        gtk_widget_set_sensitive(sync_recents_btn, FALSE);
    }
    return G_SOURCE_REMOVE;
}

static gboolean recents_sync_complete_cb(gpointer data) {
    gboolean success = GPOINTER_TO_INT(data);
    syncing_recents = FALSE;
    if (recents_spinner) {
        gtk_spinner_stop(GTK_SPINNER(recents_spinner));
        gtk_widget_hide(recents_spinner);
    }
    if (success) {
        save_recents_to_csv();  // CSV'ye kaydet
        refresh_recents_view();
        log_msg("✓ Son görüşmeler güncellendi");
    } else {
        log_msg("⚠️ Son görüşmeler alınamadı");
    }
    update_ui();
    return G_SOURCE_REMOVE;
}

static gpointer sync_recents_thread(gpointer data) {
    (void)data;
    
    g_idle_add(recents_sync_start_cb, NULL);
    
    if (!device_addr[0]) {
        log_msg("⚠️ Telefon adresi yok");
        g_idle_add(recents_sync_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }

    if (!ensure_obexd_running()) {
        log_msg("⚠️ obexd bulunamadı");
        g_idle_add(recents_sync_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }

    recent_count = 0;
    gboolean any_success = FALSE;
    
    const char *phonebooks[] = {"ich", "och", "mch"};
    const char *types[] = {"📥 Gelen", "📤 Giden", "❌ Cevapsız"};
    
    // TEK session oluştur - tüm phonebook'lar için
    GError *error = NULL;
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "Target", g_variant_new_string("PBAP"));

    GVariant *result = g_dbus_connection_call_sync(
        obex_conn, "org.bluez.obex", "/org/bluez/obex",
        "org.bluez.obex.Client1", "CreateSession",
        g_variant_new("(sa{sv})", device_addr, &opts),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 15000, NULL, &error);

    if (error || !result) {
        if (error) g_error_free(error);
        log_msg("⚠️ PBAP session kurulamadı");
        g_idle_add(recents_sync_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }

    const gchar *session_path = NULL;
    g_variant_get(result, "(&o)", &session_path);
    gchar *session_copy = g_strdup(session_path);
    g_variant_unref(result);
    
    // PhonebookAccess1 hazır olana kadar bekle
    gboolean ready = FALSE;
    for (int i = 0; i < 15; i++) {
        g_usleep(100 * 1000);
        GVariant *intr = g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", session_copy,
            "org.freedesktop.DBus.Introspectable", "Introspect",
            NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        if (intr) {
            const gchar *xml = NULL;
            g_variant_get(intr, "(&s)", &xml);
            if (xml && strstr(xml, "PhonebookAccess1")) ready = TRUE;
            g_variant_unref(intr);
            if (ready) break;
        }
    }
    
    if (!ready) {
        log_msg("⚠️ PhonebookAccess1 hazır değil");
        g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", "/org/bluez/obex",
            "org.bluez.obex.Client1", "RemoveSession",
            g_variant_new("(o)", session_copy), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        g_free(session_copy);
        g_idle_add(recents_sync_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }

    for (int pb = 0; pb < 3; pb++) {
        // Select phonebook
        error = NULL;
        g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", session_copy,
            "org.bluez.obex.PhonebookAccess1", "Select",
            g_variant_new("(ss)", "int", phonebooks[pb]),
            NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
        
        if (error) {
            g_error_free(error);
            error = NULL;
            // Boş location dene
            g_dbus_connection_call_sync(
                obex_conn, "org.bluez.obex", session_copy,
                "org.bluez.obex.PhonebookAccess1", "Select",
                g_variant_new("(ss)", "", phonebooks[pb]),
                NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
            if (error) {
                g_error_free(error);
                continue;  // session_copy'yi serbest bırakma, döngü devam etsin
            }
        }
        
        // PullAll - son 33 kayıt (3 kategori x 33 = ~100 toplam)
        GVariantBuilder pull_opts;
        g_variant_builder_init(&pull_opts, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&pull_opts, "{sv}", "Format", g_variant_new_string("vcard21"));
        g_variant_builder_add(&pull_opts, "{sv}", "MaxListCount", g_variant_new_uint16(33));
        
        error = NULL;
        result = g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", session_copy,
            "org.bluez.obex.PhonebookAccess1", "PullAll",
            g_variant_new("(sa{sv})", "", &pull_opts),
            G_VARIANT_TYPE("(oa{sv})"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);
        
        if (error || !result) {
            if (error) g_error_free(error);
            g_free(session_copy);
            continue;
        }
        
        const gchar *transfer_path = NULL;
        GVariant *props = NULL;
        g_variant_get(result, "(&o@a{sv})", &transfer_path, &props);
        
        gchar *filename = NULL;
        if (props) {
            GVariant *fname_var = g_variant_lookup_value(props, "Filename", G_VARIANT_TYPE_STRING);
            if (fname_var) {
                filename = g_strdup(g_variant_get_string(fname_var, NULL));
                g_variant_unref(fname_var);
            }
            g_variant_unref(props);
        }
        
        if (transfer_path) {
            gchar *tpath = g_strdup(transfer_path);
            g_variant_unref(result);
            
            // Transfer tamamlanana kadar bekle (max 3 saniye)
            for (int i = 0; i < 60; i++) {
                g_usleep(50 * 1000);
                GVariant *status_var = g_dbus_connection_call_sync(
                    obex_conn, "org.bluez.obex", tpath,
                    "org.freedesktop.DBus.Properties", "Get",
                    g_variant_new("(ss)", "org.bluez.obex.Transfer1", "Status"),
                    G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
                
                if (status_var) {
                    GVariant *inner;
                    g_variant_get(status_var, "(v)", &inner);
                    const gchar *status = g_variant_get_string(inner, NULL);
                    gboolean complete = (status && g_strcmp0(status, "complete") == 0);
                    g_variant_unref(inner);
                    g_variant_unref(status_var);
                    
                    if (complete) {
                        if (!filename) {
                            GVariant *file_var = g_dbus_connection_call_sync(
                                obex_conn, "org.bluez.obex", tpath,
                                "org.freedesktop.DBus.Properties", "Get",
                                g_variant_new("(ss)", "org.bluez.obex.Transfer1", "Filename"),
                                G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
                            if (file_var) {
                                GVariant *inner_file;
                                g_variant_get(file_var, "(v)", &inner_file);
                                filename = g_strdup(g_variant_get_string(inner_file, NULL));
                                g_variant_unref(inner_file);
                                g_variant_unref(file_var);
                            }
                        }
                        break;
                    }
                }
            }
            g_free(tpath);
        } else {
            g_variant_unref(result);
        }
        
        if (filename) {
            parse_vcf_recents(filename, types[pb]);
            g_free(filename);
            any_success = TRUE;
        }
    }
    
    // Tüm kayıtları zamana göre sırala (en yeniden en eskiye)
    if (recent_count > 1) {
        qsort(recent_entries, recent_count, sizeof(RecentEntry), compare_recents);
    }
    
    // Son 100 kayıtla sınırla
    if (recent_count > 100) {
        recent_count = 100;
    }
    
    // Session'ı kapat (döngü bittikten sonra)
    if (session_copy && session_copy[0]) {
        g_dbus_connection_call_sync(
            obex_conn, "org.bluez.obex", "/org/bluez/obex",
            "org.bluez.obex.Client1", "RemoveSession",
            g_variant_new("(o)", session_copy),
            NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    }
    g_free(session_copy);

    g_idle_add(recents_sync_complete_cb, GINT_TO_POINTER(any_success));
    return NULL;
}

static void on_sync_recents_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    if (current_state != STATE_CONNECTED) {
        log_msg("⚠️ Son görüşmeler için telefon bağlı olmalı");
        return;
    }
    if (syncing_recents) {
        log_msg("⚠️ Son görüşmeler zaten alınıyor...");
        return;
    }
    log_msg("📥 Son görüşmeler alınıyor...");
    g_thread_new("pbap_recents", sync_recents_thread, NULL);
}

// HFP monitoring thread - bağlantıyı açık tutar ve olayları dinler
static gboolean hfp_monitor_running = FALSE;
static GThread *hfp_monitor_thread_handle = NULL;

// Sadece UI'ı refresh et (state değiştirmeden)
static gboolean hfp_refresh_ui_cb(gpointer data) {
    (void)data;
    update_call_ui();
    return G_SOURCE_REMOVE;
}

static gboolean restart_incoming_listener_cb(gpointer data) {
    (void)data;
    if (current_state == STATE_CONNECTED && !incoming_call_thread) {
        log_msg("🔁 Dinleyici yeniden başlatılıyor");
        start_incoming_call_listener();
    }
    return G_SOURCE_REMOVE;
}

static gboolean hfp_update_call_state_cb(gpointer data) {
    CallState new_state = GPOINTER_TO_INT(data);
    
    // Sadece IDLE durumuna geçerken sıfırla
    if (new_state == CALL_IDLE) {
        stop_sco_audio(NULL);
        clear_call_info();
    }
    
    set_call_state(new_state);
    update_ui();
    return G_SOURCE_REMOVE;
}

static void handle_ciev_event(int ind, int val) {
    if (ind == 1) {  // Call indicator
        if (val == 1) {
            log_msg("✓ Arama aktif");
            if (!sco_audio_running && sco_socket < 0) {
                sco_connect();
            }
            g_idle_add(hfp_update_call_state_cb, GINT_TO_POINTER(CALL_ACTIVE));
        } else if (val == 0) {
            if (current_call_state != CALL_IDLE) {
                log_msg("📱 Arama sonlandı");
                g_idle_add(hfp_update_call_state_cb, GINT_TO_POINTER(CALL_IDLE));
            }
        }
        return;
    }

    if (ind == 2) {  // Call setup indicator
        if (val == 0) {
            if (current_call_state == CALL_OUTGOING || current_call_state == CALL_RINGING) {
                log_msg("✓ Arama kurulumu tamamlandı (setup=0)");
            }
            // Setup bitti ama aktif arama yoksa UI'ı temizle
            if (current_call_state != CALL_ACTIVE) {
                g_idle_add(hfp_update_call_state_cb, GINT_TO_POINTER(CALL_IDLE));
            }
            return;
        }
        if (val == 1) {
            if (current_call_state != CALL_RINGING) {
                log_msg("🔔 GELEN ARAMA (CIEV)");
                g_idle_add(hfp_update_call_state_cb, GINT_TO_POINTER(CALL_RINGING));
            }
            return;
        }
        if (val == 2 || val == 3) {
            if (current_call_state != CALL_OUTGOING) {
                log_msg("📱 Giden arama (CIEV)");
                g_idle_add(hfp_update_call_state_cb, GINT_TO_POINTER(CALL_OUTGOING));
            }
            if (!sco_audio_running && sco_socket < 0) {
                sco_connect();
            }
            return;
        }
    }
}

static gpointer hfp_monitor_thread(gpointer data) {
    (void)data;
    char buf[512];
    int n;
    
    log_msg("🔊 HFP monitor başlatıldı");
    
    while (hfp_monitor_running) {
        int sock = hfp_socket;  // Local copy
        if (sock < 0) break;
        
        // Non-blocking check
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 500000;  // 500ms timeout
        
        int ret = select(sock + 1, &readfds, NULL, NULL, &tv);
        
        // Durduruldu mu kontrol et
        if (!hfp_monitor_running) break;
        
        // Socket hala geçerli mi?
        if (hfp_socket < 0) break;
        
        if (ret > 0 && FD_ISSET(sock, &readfds)) {
            memset(buf, 0, sizeof(buf));
            n = read(sock, buf, sizeof(buf) - 1);
            
            if (n <= 0) {
                // Bağlantı kesildi
                if (hfp_monitor_running) {
                    log_msg("⚠️ HFP bağlantısı kesildi");
                    g_idle_add(hfp_update_call_state_cb, GINT_TO_POINTER(CALL_IDLE));
                }
                if (hfp_socket >= 0) {
                    shutdown(hfp_socket, SHUT_RDWR);
                    close(hfp_socket);
                    hfp_socket = -1;
                }
                break;
            }
            
            // AT olaylarını parse et
            if (strstr(buf, "+CIEV")) {
                int ind = -1, val = -1;
                if (parse_ciev(buf, &ind, &val)) {
                    handle_ciev_event(ind, val);
                }
            } else if (strstr(buf, "NO CARRIER") || strstr(buf, "BUSY") || strstr(buf, "NO ANSWER")) {
                log_msg("📱 Arama sonlandı");
                g_idle_add(hfp_update_call_state_cb, GINT_TO_POINTER(CALL_IDLE));
                break;
            }
        } else if (ret < 0 && errno != EINTR) {
            break;
        }
    }
    
    log_msg("🔊 HFP monitor durduruldu");
    hfp_monitor_running = FALSE;
    return NULL;
}

// HFP bağlantısını kapat (thread-safe)
static void hfp_close(void) {
    // Önce monitor'u durdur
    hfp_monitor_running = FALSE;
    sco_audio_running = FALSE;  // Audio thread'leri durdur
    
    // SCO socket'i kapat - bu thread'lerin bitmesini tetikler
    if (sco_socket >= 0) {
        shutdown(sco_socket, SHUT_RDWR);
        close(sco_socket);
        sco_socket = -1;
        log_msg("🔊 SCO audio kapatıldı");
    }
    
    // Thread'lerin kendi PulseAudio'larını kapatmasını bekle
    usleep(100000);  // 100ms bekle
    
    // Socket'i kapat
    if (hfp_socket >= 0) {
        shutdown(hfp_socket, SHUT_RDWR);  // select'i uyandır
        close(hfp_socket);
        hfp_socket = -1;
    }
    
    // Thread'in bitmesini bekle (max 1 saniye)
    if (hfp_monitor_thread_handle) {
        g_thread_join(hfp_monitor_thread_handle);
        hfp_monitor_thread_handle = NULL;
    }
    
    log_msg("✓ HFP bağlantısı kapatıldı");
}

// Lambda helper for thread-safe logging
static gboolean lambda_log(char *msg) {
    log_msg(msg);
    g_free(msg);
    return FALSE;
}

// Gelen aramayı cevapla callback
static gboolean answer_incoming_call_cb(gpointer data) {
    (void)data;
    
    if (hfp_listen_socket >= 0) {
        // ATA komutu gönder - aramayı cevapla
        char cmd[] = "ATA\r";
        if (write(hfp_listen_socket, cmd, strlen(cmd)) > 0) {
            usleep(200000);
            char buf[256] = {0};
            read(hfp_listen_socket, buf, sizeof(buf) - 1);
            log_msg("✓ Arama cevaplandı");
            
            // SCO bağlantısı kur
            sco_connect();
            
            set_call_state(CALL_ACTIVE);
        }
    }
    return FALSE;
}

// Gelen aramayı reddet callback
static gboolean reject_incoming_call_cb(gpointer data) {
    (void)data;
    
    if (hfp_listen_socket >= 0) {
        // AT+CHUP - aramayı reddet
        char cmd[] = "AT+CHUP\r";
        if (write(hfp_listen_socket, cmd, strlen(cmd)) > 0) {
            usleep(200000);
            char buf[256] = {0};
            read(hfp_listen_socket, buf, sizeof(buf) - 1);
            log_msg("📱 Arama reddedildi");
        }
    }
    set_call_state(CALL_IDLE);
    current_call_number[0] = '\0';
    current_call_name[0] = '\0';
    return FALSE;
}

// Gelen arama dinleyici thread
static gpointer incoming_call_listener(gpointer data) {
    (void)data;
    
    log_msg("📞 Gelen arama dinleyici başlatıldı");
    
    // HFP bağlantısı kur
    hfp_listen_socket = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (hfp_listen_socket < 0) {
        log_msg("⚠️ Dinleyici soket hatası");
        return NULL;
    }
    
    struct sockaddr_rc addr = {0};
    addr.rc_family = AF_BLUETOOTH;
    addr.rc_channel = hfp_channel ? hfp_channel : 3;  // Dinamik veya varsayılan
    str2ba(device_addr, &addr.rc_bdaddr);
    
    if (connect(hfp_listen_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg("⚠️ Dinleyici bağlantı hatası");
        close(hfp_listen_socket);
        hfp_listen_socket = -1;
        return NULL;
    }
    
    // SLC handshake
    char buf[512];
    char cmd[64];
    
    // AT+BRSF
    snprintf(cmd, sizeof(cmd), "AT+BRSF=1\r");
    write(hfp_listen_socket, cmd, strlen(cmd));
    usleep(100000);
    read(hfp_listen_socket, buf, sizeof(buf) - 1);
    
    // AT+CIND=?
    snprintf(cmd, sizeof(cmd), "AT+CIND=?\r");
    write(hfp_listen_socket, cmd, strlen(cmd));
    usleep(100000);
    read(hfp_listen_socket, buf, sizeof(buf) - 1);
    
    // AT+CIND?
    snprintf(cmd, sizeof(cmd), "AT+CIND?\r");
    write(hfp_listen_socket, cmd, strlen(cmd));
    usleep(100000);
    read(hfp_listen_socket, buf, sizeof(buf) - 1);
    
    // AT+CMER - event reporting aktif
    snprintf(cmd, sizeof(cmd), "AT+CMER=3,0,0,1\r");
    write(hfp_listen_socket, cmd, strlen(cmd));
    usleep(100000);
    read(hfp_listen_socket, buf, sizeof(buf) - 1);
    
    // AT+CLIP - arayan numara gösterimi aktif
    snprintf(cmd, sizeof(cmd), "AT+CLIP=1\r");
    write(hfp_listen_socket, cmd, strlen(cmd));
    usleep(100000);
    read(hfp_listen_socket, buf, sizeof(buf) - 1);
    
    log_msg("✓ Gelen arama dinleyici hazır");
    
    // Olayları dinle
    while (incoming_call_running && hfp_listen_socket >= 0) {
        if (hfp_listen_paused) {
            usleep(100000);
            continue;
        }
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(hfp_listen_socket, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(hfp_listen_socket + 1, &readfds, NULL, NULL, &tv);
        
        if (!incoming_call_running) break;

        if (hfp_listen_paused) {
            continue;
        }
        
        if (ret > 0 && FD_ISSET(hfp_listen_socket, &readfds)) {
            memset(buf, 0, sizeof(buf));
            ssize_t n = read(hfp_listen_socket, buf, sizeof(buf) - 1);
            
            if (n <= 0) {
                log_msg("⚠️ Dinleyici bağlantısı kesildi");
                g_idle_add(hfp_update_call_state_cb, GINT_TO_POINTER(CALL_IDLE));
                g_idle_add(restart_incoming_listener_cb, NULL);
                break;
            }
            
            // Debug: gelen veriyi logla
            char debug_msg[128];
            snprintf(debug_msg, sizeof(debug_msg), "📥 HFP: %.60s", buf);
            log_msg(debug_msg);
            
            // +CLIP ile numara al (RING'den ayrı gelebilir)
            char *clip = strstr(buf, "+CLIP:");
            if (clip) {
                // +CLIP: "numara",129,,,"isim" formatı
                char *num_start = strchr(clip, '"');
                if (num_start) {
                    num_start++;
                    char *num_end = strchr(num_start, '"');
                    if (num_end) {
                        *num_end = '\0';
                        strncpy(current_call_number, num_start, sizeof(current_call_number) - 1);
                        
                        // İsmi +CLIP'den al (5. tırnak içinde)
                        current_call_name[0] = '\0';
                        char *name_search = num_end + 1;
                        int quote_count = 0;
                        char *name_start = NULL;
                        
                        while (*name_search && quote_count < 4) {
                            if (*name_search == '"') {
                                quote_count++;
                                if (quote_count == 4) {
                                    name_start = name_search + 1;
                                }
                            }
                            name_search++;
                        }
                        
                        if (name_start) {
                            char *name_end = strchr(name_start, '"');
                            if (name_end && name_end > name_start) {
                                *name_end = '\0';
                                strncpy(current_call_name, name_start, sizeof(current_call_name) - 1);
                            }
                        }
                        
                        // İsim +CLIP'den gelmezse rehberden bul
                        if (!current_call_name[0]) {
                            char normalized_incoming[32] = {0};
                            int incoming_len = strlen(current_call_number);
                            if (incoming_len >= 10) {
                                strncpy(normalized_incoming, current_call_number + incoming_len - 10, 10);
                            } else {
                                strncpy(normalized_incoming, current_call_number, sizeof(normalized_incoming) - 1);
                            }
                            
                            for (int i = 0; i < all_contacts_count; i++) {
                                char normalized_contact[32] = {0};
                                int contact_len = strlen(all_contacts[i].number);
                                if (contact_len >= 10) {
                                    strncpy(normalized_contact, all_contacts[i].number + contact_len - 10, 10);
                                } else {
                                    strncpy(normalized_contact, all_contacts[i].number, sizeof(normalized_contact) - 1);
                                }
                                
                                if (strcmp(normalized_incoming, normalized_contact) == 0) {
                                    strncpy(current_call_name, all_contacts[i].name, sizeof(current_call_name) - 1);
                                    break;
                                }
                            }
                        }
                        
                        char msg[256];
                        if (current_call_name[0]) {
                            snprintf(msg, sizeof(msg), "📱 Arayan: %s (%s)", current_call_name, current_call_number);
                        } else {
                            snprintf(msg, sizeof(msg), "📱 Arayan: %s", current_call_number);
                        }
                        log_msg(msg);
                        
                        // UI'ı güncelle (zaten RINGING ise sadece refresh)
                        if (current_call_state == CALL_RINGING) {
                            g_idle_add(hfp_refresh_ui_cb, NULL);
                        } else {
                            g_idle_add(hfp_update_call_state_cb, GINT_TO_POINTER(CALL_RINGING));
                        }
                    }
                }
            }
            
            // RING - gelen arama (numara olmadan da olabilir)
            if (strstr(buf, "RING") && current_call_state != CALL_RINGING) {
                log_msg("🔔 GELEN ARAMA!");
                g_idle_add(hfp_update_call_state_cb, GINT_TO_POINTER(CALL_RINGING));
            }
            
            // CIEV olayları: ind=1 (call), ind=2 (callsetup)
            if (strstr(buf, "+CIEV:")) {
                int ind = -1, val = -1;
                if (parse_ciev(buf, &ind, &val)) {
                    handle_ciev_event(ind, val);
                }
            }
            else if (strstr(buf, "NO CARRIER") || strstr(buf, "BUSY") || strstr(buf, "NO ANSWER") || strstr(buf, "ERROR")) {
                log_msg("📱 Arama sonlandı");
                g_idle_add(hfp_update_call_state_cb, GINT_TO_POINTER(CALL_IDLE));
            }
        }
    }
    
    if (hfp_listen_socket >= 0) {
        close(hfp_listen_socket);
        hfp_listen_socket = -1;
    }

    incoming_call_running = FALSE;
    incoming_call_thread = NULL;

    log_msg("📞 Gelen arama dinleyici durdu");
    return NULL;
}

// Gelen arama dinleyiciyi başlat
static void start_incoming_call_listener(void) {
    if (incoming_call_thread) return;  // Zaten çalışıyor
    if (!device_addr[0]) return;  // Cihaz yok
    
    incoming_call_running = TRUE;
    incoming_call_thread = g_thread_new("incoming-call", incoming_call_listener, NULL);
}

// Gelen arama dinleyiciyi durdur
static void stop_incoming_call_listener(void) {
    incoming_call_running = FALSE;
    
    if (hfp_listen_socket >= 0) {
        shutdown(hfp_listen_socket, SHUT_RDWR);
        close(hfp_listen_socket);
        hfp_listen_socket = -1;
    }
    
    if (incoming_call_thread) {
        g_thread_join(incoming_call_thread);
        incoming_call_thread = NULL;
    }
}

static void aec_fifo_clear(void) {
    pthread_mutex_lock(&aec_fifo_mutex);
    aec_fifo_head = 0;
    aec_fifo_tail = 0;
    aec_fifo_size = 0;
    pthread_mutex_unlock(&aec_fifo_mutex);
}

static void aec_fifo_push(const int16_t *samples, int count) {
    if (count <= 0) return;
    pthread_mutex_lock(&aec_fifo_mutex);
    for (int i = 0; i < count; i++) {
        aec_render_fifo[aec_fifo_head] = samples[i];
        aec_fifo_head = (aec_fifo_head + 1) % AEC_FIFO_CAPACITY;
        if (aec_fifo_size < AEC_FIFO_CAPACITY) {
            aec_fifo_size++;
        } else {
            aec_fifo_tail = (aec_fifo_tail + 1) % AEC_FIFO_CAPACITY;
        }
    }
    pthread_mutex_unlock(&aec_fifo_mutex);
}

static gboolean aec_fifo_pop(int16_t *out, int count) {
    if (count <= 0) return FALSE;
    gboolean ok = FALSE;
    pthread_mutex_lock(&aec_fifo_mutex);
    if (aec_fifo_size >= count) {
        for (int i = 0; i < count; i++) {
            out[i] = aec_render_fifo[aec_fifo_tail];
            aec_fifo_tail = (aec_fifo_tail + 1) % AEC_FIFO_CAPACITY;
        }
        aec_fifo_size -= count;
        ok = TRUE;
    }
    pthread_mutex_unlock(&aec_fifo_mutex);
    return ok;
}

static void init_webrtc_aec(void) {
#ifdef HAVE_WEBRTC_APM
    if (aec_force_disable) {
        aec_enabled = FALSE;
        aec_fifo_clear();
        log_msg("⚠️ WebRTC AEC kapalı (robotik ses önleme)");
        return;
    }
    pthread_mutex_lock(&aec_mutex);
    if (!aec_handle) {
        aec_handle = aec_create(8000);
        aec_enabled = (aec_handle != NULL);
    } else {
        aec_enabled = TRUE;
    }
    pthread_mutex_unlock(&aec_mutex);
#else
    aec_enabled = FALSE;
#endif
    if (aec_enabled) {
        log_msg("✅ WebRTC AEC aktif");
    } else {
        log_msg("⚠️ WebRTC AEC devre dışı");
    }
    aec_fifo_clear();
}

static void shutdown_webrtc_aec(void) {
#ifdef HAVE_WEBRTC_APM
    pthread_mutex_lock(&aec_mutex);
    if (aec_handle) {
        aec_destroy(aec_handle);
        aec_handle = NULL;
    }
    pthread_mutex_unlock(&aec_mutex);
#endif
    aec_enabled = FALSE;
    aec_fifo_clear();
}

// SCO -> PulseAudio playback thread (telefon sesini PC'ye)
static void* sco_playback_thread_func(void *data) {
    (void)data;
    
    // PulseAudio format: 8kHz mono 16-bit (SCO standart)
    pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = 8000,
        .channels = 1
    };
    
    int err;
    pulse_playback = pa_simple_new(
        NULL,               // default server
        "BT Headset",       // app name
        PA_STREAM_PLAYBACK,
        NULL,               // default device
        "Telefon Sesi",     // stream description
        &ss,
        NULL,               // default channel map
        NULL,               // default buffering
        &err
    );
    
    if (!pulse_playback) {
        g_idle_add((GSourceFunc)lambda_log, g_strdup_printf("⚠️ Hoparlör açılamadı: %s", pa_strerror(err)));
        return NULL;
    }
    
    g_idle_add((GSourceFunc)lambda_log, g_strdup("🔊 Hoparlör aktif - telefon sesi geliyor"));
    
    unsigned char buf[240];
    ssize_t bytes_read;
    
    while (sco_audio_running && sco_socket >= 0) {
        bytes_read = recv(sco_socket, buf, sizeof(buf), 0);
        if (bytes_read <= 0) {
            if (sco_audio_running) {
                g_idle_add((GSourceFunc)lambda_log, g_strdup("⚠️ Telefon sesi kesildi"));
            }
            break;
        }

        if (aec_enabled) {
            int16_t *samples = (int16_t *)buf;
            int count = (int)(bytes_read / 2);
            aec_fifo_push(samples, count);
        }
        
        if (pa_simple_write(pulse_playback, buf, bytes_read, &err) < 0) {
            g_idle_add((GSourceFunc)lambda_log, g_strdup_printf("⚠️ Ses yazma hatası: %s", pa_strerror(err)));
            break;
        }
    }
    
    if (pulse_playback) {
        pa_simple_drain(pulse_playback, NULL);
        pa_simple_free(pulse_playback);
        pulse_playback = NULL;
    }
    
    g_idle_add((GSourceFunc)lambda_log, g_strdup("🔇 Hoparlör kapatıldı"));
    return NULL;
}

// PulseAudio -> SCO capture thread (PC mikrofonunu telefona)
static void* sco_capture_thread_func(void *data) {
    (void)data;
    
    // PulseAudio format: 8kHz mono 16-bit (SCO standart)
    pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = 8000,
        .channels = 1
    };
    
    int err;
    pulse_capture = pa_simple_new(
        NULL,               // default server
        "BT Headset",       // app name
        PA_STREAM_RECORD,
        NULL,               // default device (mikrofon)
        "PC Mikrofon",      // stream description
        &ss,
        NULL,               // default channel map
        NULL,               // default buffering
        &err
    );
    
    if (!pulse_capture) {
        g_idle_add((GSourceFunc)lambda_log, g_strdup_printf("⚠️ Mikrofon açılamadı: %s", pa_strerror(err)));
        return NULL;
    }
    
    g_idle_add((GSourceFunc)lambda_log, g_strdup("🎤 Mikrofon aktif - sesiniz telefona gidiyor"));
    
    const int mtu = sco_mtu;  // Dinamik MTU
    unsigned char buf[AEC_FRAME_BYTES];
    int16_t render_frame[AEC_FRAME_SAMPLES];
    int send_error_logged = 0;
    
    while (sco_audio_running && sco_socket >= 0) {
        int read_bytes = aec_enabled ? AEC_FRAME_BYTES : mtu;

        // Mikrofondan oku
        if (pa_simple_read(pulse_capture, buf, read_bytes, &err) < 0) {
            if (sco_audio_running) {
                g_idle_add((GSourceFunc)lambda_log, g_strdup_printf("⚠️ Mikrofon okuma hatası: %s", pa_strerror(err)));
            }
            break;
        }

        if (aec_enabled) {
            int16_t *near = (int16_t *)buf;
            if (!aec_fifo_pop(render_frame, AEC_FRAME_SAMPLES)) {
                memset(render_frame, 0, sizeof(render_frame));
            }
#ifdef HAVE_WEBRTC_APM
            pthread_mutex_lock(&aec_mutex);
            if (aec_handle) {
                aec_process(aec_handle, near, render_frame, AEC_FRAME_SAMPLES);
            }
            pthread_mutex_unlock(&aec_mutex);
#endif
        }
        
        // SCO'ya gönder - MTU boyutunda parçalara böl
        for (int offset = 0; offset < read_bytes; offset += mtu) {
            int chunk = read_bytes - offset;
            if (chunk > mtu) chunk = mtu;
            ssize_t sent = send(sco_socket, buf + offset, chunk, MSG_NOSIGNAL);
            if (sent <= 0) {
                if (errno == EPIPE || errno == ENOTCONN || errno == ECONNRESET) {
                    if (!send_error_logged) {
                        g_idle_add((GSourceFunc)lambda_log, g_strdup_printf("⚠️ Mikrofon gönderme hatası: %s", strerror(errno)));
                        send_error_logged = 1;
                    }
                    stop_sco_audio("🔇 SCO kapatıldı (karşı uç kapattı)");
                    return NULL;
                }
                if (sco_audio_running && errno != EAGAIN && errno != EWOULDBLOCK && !send_error_logged) {
                    g_idle_add((GSourceFunc)lambda_log, g_strdup_printf("⚠️ Mikrofon gönderme hatası: %s", strerror(errno)));
                    send_error_logged = 1;
                }
                usleep(1000);  // Kısa bekle ve tekrar dene
                continue;
            }
        }
    }
    
    if (pulse_capture) {
        pa_simple_free(pulse_capture);
        pulse_capture = NULL;
    }
    
    g_idle_add((GSourceFunc)lambda_log, g_strdup("🔇 Mikrofon kapatıldı"));
    return NULL;
}

// SCO audio bağlantısı kur
static gboolean sco_connect(void) {
    if (!device_addr[0]) {
        return FALSE;
    }

    // Önceki bağlantı hala açıksa bekle
    if (sco_audio_running) {
        log_msg("ℹ️ Önceki SCO kapatılıyor...");
        stop_sco_audio(NULL);
        usleep(100000);  // 100ms ekstra bekle
    }

    if (sco_socket >= 0) {
        // Socket açık ama thread yok - temizle
        shutdown(sco_socket, SHUT_RDWR);
        close(sco_socket);
        sco_socket = -1;
        usleep(50000);
    }
    
    // SCO socket oluştur
    sco_socket = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
    if (sco_socket < 0) {
        log_msg("⚠️ SCO soket hatası");
        return FALSE;
    }
    
    // SCO ses kalitesi ayarları (mSBC için)
    struct bt_voice voice = { .setting = BT_VOICE_CVSD_16BIT };  // veya BT_VOICE_TRANSPARENT
    if (setsockopt(sco_socket, SOL_BLUETOOTH, BT_VOICE, &voice, sizeof(voice)) < 0) {
        // Hata olsa bile devam et - bazı sistemlerde desteklenmez
        char msg[128];
        snprintf(msg, sizeof(msg), "ℹ️ SCO voice ayarı: %s", strerror(errno));
        log_msg(msg);
    }
    
    // Bağlantı adresi
    struct sockaddr_sco addr = {0};
    addr.sco_family = AF_BLUETOOTH;
    str2ba(device_addr, &addr.sco_bdaddr);
    
    // Bağlan
    log_msg("🔊 SCO audio bağlanıyor...");
    if (connect(sco_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "⚠️ SCO bağlantı hatası: %s", strerror(errno));
        log_msg(msg);
        close(sco_socket);
        sco_socket = -1;
        return FALSE;
    }
    
    log_msg("✓ SCO audio bağlandı");

    // SCO MTU'yu dinamik oku
    struct sco_options sco_opts;
    socklen_t optlen = sizeof(sco_opts);
    if (getsockopt(sco_socket, SOL_SCO, SCO_OPTIONS, &sco_opts, &optlen) == 0) {
        sco_mtu = sco_opts.mtu;
        char msg[64];
        snprintf(msg, sizeof(msg), "ℹ️ SCO MTU: %d byte", sco_mtu);
        log_msg(msg);
    } else {
        sco_mtu = 48;  // Varsayılan
        log_msg("ℹ️ SCO MTU okunamadı, varsayılan: 48");
    }

    init_webrtc_aec();
    
    // Playback thread'i başlat (telefon -> PC hoparlör)
    sco_audio_running = TRUE;
    if (pthread_create(&sco_playback_thread, NULL, sco_playback_thread_func, NULL) != 0) {
        log_msg("⚠️ Hoparlör thread hatası");
    }
    
    // Capture thread'i başlat (PC mikrofon -> telefon)
    if (pthread_create(&sco_capture_thread, NULL, sco_capture_thread_func, NULL) != 0) {
        log_msg("⚠️ Mikrofon thread hatası");
    }
    
    return TRUE;
}

// HFP üzerinden aramayı bitir
static void hfp_hangup(void) {
    hfp_monitor_running = FALSE;  // Önce monitor'u durdur
    
    if (hfp_socket >= 0) {
        // AT+CHUP gönder
        char cmd[] = "AT+CHUP\r";
        if (write(hfp_socket, cmd, strlen(cmd)) > 0) {
            usleep(200000);
            char buf[256] = {0};
            read(hfp_socket, buf, sizeof(buf) - 1);
        }
        log_msg("📱 Arama sonlandırıldı");
    }
    
    hfp_close();
    set_call_state(CALL_IDLE);
    current_call_number[0] = '\0';
    current_call_name[0] = '\0';
}

// Numarayı ara - numarayı panoya kopyala ve bildirim göster
// HFP üzerinden arama başlat (RFCOMM)
static gboolean hfp_dial(const char *number) {
    if (!device_addr[0]) {
        log_msg("⚠️ Telefon adresi yok");
        return FALSE;
    }
    
    // Dinleyici soketi varsa onu kullan
    if (hfp_listen_socket >= 0) {
        log_msg("📱 Mevcut HFP bağlantısı kullanılıyor...");

        hfp_listen_paused = TRUE;
        usleep(150000);
        
        // Aramayı başlat
        char cmd[128];
        char buf[512];
        
        snprintf(cmd, sizeof(cmd), "ATD%s;\r", number);
        if (write(hfp_listen_socket, cmd, strlen(cmd)) <= 0) {
            log_msg("⚠️ Arama komutu gönderilemedi");
            hfp_listen_paused = FALSE;
            return FALSE;
        }
        
        usleep(500000);  // 500ms bekle
        memset(buf, 0, sizeof(buf));
        int n = read(hfp_listen_socket, buf, sizeof(buf) - 1);
        
        // Debug log
        char debug_msg[600];
        snprintf(debug_msg, sizeof(debug_msg), "📥 HFP yanıt (%d byte): [%s]", n, buf);
        log_msg(debug_msg);
        
        // OK, +CIEV (call setup indicator), veya CONNECT = başarılı
        if (strstr(buf, "OK") || strstr(buf, "+CIEV") || strstr(buf, "CONNECT")) {
            char msg[128];
            snprintf(msg, sizeof(msg), "✓ Arama başlatıldı: %s", number);
            log_msg(msg);
            
            // SCO bağlantısı kur
            sco_connect();
            
            set_call_state(CALL_OUTGOING);
            hfp_listen_paused = FALSE;
            return TRUE;
        } else if (strstr(buf, "ERROR") || strstr(buf, "NO CARRIER")) {
            log_msg("⚠️ Telefon aramayı reddetti");
            hfp_listen_paused = FALSE;
            return FALSE;
        } else {
            log_msg("⚠️ Bilinmeyen yanıt, yine de deneniyor...");
            // Bilinmeyen yanıtta da dene
            sco_connect();
            set_call_state(CALL_OUTGOING);
            hfp_listen_paused = FALSE;
            return TRUE;
        }
    }
    
    // Dinleyici yoksa yeni bağlantı kur
    hfp_close();
    
    // RFCOMM soketi oluştur
    hfp_socket = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (hfp_socket < 0) {
        log_msg("⚠️ RFCOMM soket hatası");
        return FALSE;
    }
    
    // Bağlantı adresi
    struct sockaddr_rc addr = {0};
    addr.rc_family = AF_BLUETOOTH;
    addr.rc_channel = hfp_channel ? hfp_channel : 3;  // Dinamik veya varsayılan
    str2ba(device_addr, &addr.rc_bdaddr);
    
    // Bağlan
    log_msg("📱 HFP bağlantısı kuruluyor...");
    if (connect(hfp_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(hfp_socket);
        hfp_socket = -1;
        log_msg("⚠️ HFP bağlantısı kurulamadı");
        return FALSE;
    }
    
    log_msg("✓ HFP bağlantısı kuruldu");
    
    char cmd[128];
    char buf[512];
    int n;
    
    // HFP SLC (Service Level Connection) Handshake
    // 1. AT+BRSF - Özellik değişimi
    snprintf(cmd, sizeof(cmd), "AT+BRSF=0\r");
    if (write(hfp_socket, cmd, strlen(cmd)) < 0) goto error;
    usleep(100000);
    memset(buf, 0, sizeof(buf));
    n = read(hfp_socket, buf, sizeof(buf) - 1);
    if (n <= 0 || !strstr(buf, "OK")) {
        log_msg("⚠️ AT+BRSF hatası");
        goto error;
    }
    
    // 2. AT+CIND=? - Gösterge desteğini sor
    snprintf(cmd, sizeof(cmd), "AT+CIND=?\r");
    if (write(hfp_socket, cmd, strlen(cmd)) < 0) goto error;
    usleep(100000);
    memset(buf, 0, sizeof(buf));
    n = read(hfp_socket, buf, sizeof(buf) - 1);
    if (n <= 0 || !strstr(buf, "OK")) {
        log_msg("⚠️ AT+CIND=? hatası");
        goto error;
    }
    
    // 3. AT+CIND? - Gösterge durumunu al
    snprintf(cmd, sizeof(cmd), "AT+CIND?\r");
    if (write(hfp_socket, cmd, strlen(cmd)) < 0) goto error;
    usleep(100000);
    memset(buf, 0, sizeof(buf));
    n = read(hfp_socket, buf, sizeof(buf) - 1);
    if (n <= 0 || !strstr(buf, "OK")) {
        log_msg("⚠️ AT+CIND? hatası");
        goto error;
    }
    
    // 4. AT+CMER - Olay raporlamayı etkinleştir
    snprintf(cmd, sizeof(cmd), "AT+CMER=3,0,0,1\r");
    if (write(hfp_socket, cmd, strlen(cmd)) < 0) goto error;
    usleep(100000);
    memset(buf, 0, sizeof(buf));
    n = read(hfp_socket, buf, sizeof(buf) - 1);
    if (n <= 0 || !strstr(buf, "OK")) {
        snprintf(cmd, sizeof(cmd), "AT+CMER=3,0,0,0\r");
        if (write(hfp_socket, cmd, strlen(cmd)) < 0) goto error;
        usleep(100000);
        memset(buf, 0, sizeof(buf));
        read(hfp_socket, buf, sizeof(buf) - 1);
    }
    
    log_msg("✓ HFP SLC kuruldu");
    
    // 5. AT+NREC=0 - Noise reduction kapatma (isteğe bağlı)
    snprintf(cmd, sizeof(cmd), "AT+NREC=0\r");
    write(hfp_socket, cmd, strlen(cmd));
    usleep(100000);
    memset(buf, 0, sizeof(buf));
    read(hfp_socket, buf, sizeof(buf) - 1);
    
    // 6. ATD ile arama başlat
    snprintf(cmd, sizeof(cmd), "ATD%s;\r", number);
    if (write(hfp_socket, cmd, strlen(cmd)) < 0) goto error;
    
    // Yanıtı bekle
    usleep(1000000);
    memset(buf, 0, sizeof(buf));
    n = read(hfp_socket, buf, sizeof(buf) - 1);
    
    if (n > 0 && strstr(buf, "OK")) {
        char msg[256];
        snprintf(msg, sizeof(msg), "✓ Arama başlatıldı: %s", number);
        log_msg(msg);
        
        // 7. SCO audio bağlantısı kur - ses PC'ye gelsin
        sco_connect();
        
        // 8. BlueZ üzerinden HFP audio profile'ı bağla (yedek)
        if (device_path[0] && dbus_conn) {
            GError *error = NULL;
            // HFP Handsfree UUID
            g_dbus_connection_call_sync(
                dbus_conn, "org.bluez", device_path,
                "org.bluez.Device1", "ConnectProfile",
                g_variant_new("(s)", "0000111e-0000-1000-8000-00805f9b34fb"),
                NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
            if (error) {
                g_error_free(error);
                // Alternatif: HSP UUID dene
                g_dbus_connection_call_sync(
                    dbus_conn, "org.bluez", device_path,
                    "org.bluez.Device1", "ConnectProfile",
                    g_variant_new("(s)", "0000111f-0000-1000-8000-00805f9b34fb"),
                    NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
            }
        }
        
        // Monitor thread'i başlat - socket'i açık tutar
        hfp_monitor_running = TRUE;
        hfp_monitor_thread_handle = g_thread_new("hfp_monitor", hfp_monitor_thread, NULL);
        
        return TRUE;
    } else {
        char msg[256];
        if (n > 0 && strstr(buf, "ERROR")) {
            snprintf(msg, sizeof(msg), "⚠️ Telefon aramayı reddetti");
        } else if (n > 0 && strstr(buf, "NO CARRIER")) {
            snprintf(msg, sizeof(msg), "⚠️ Bağlantı kesildi");
        } else {
            snprintf(msg, sizeof(msg), "⚠️ Arama yanıtı: %s", n > 0 ? buf : "(boş)");
        }
        log_msg(msg);
        goto error;
    }
    
error:
    hfp_close();
    return FALSE;
}

static void dial_number(const char *number) {
    if (!number || !*number) {
        log_msg("⚠️ Numara boş");
        return;
    }
    if (current_state != STATE_CONNECTED) {
        log_msg("⚠️ Arama için telefon bağlı olmalı");
        return;
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "📞 Aranıyor: %s", number);
    log_msg(msg);
    
    // HFP ile arama dene
    if (hfp_dial(number)) {
        // Başarılı - call state güncelle
        strncpy(current_call_number, number, sizeof(current_call_number) - 1);
        current_call_name[0] = '\0';  // İsim rehberden bulunabilir
        
        // Rehberden isim bul
        for (int i = 0; i < all_contacts_count; i++) {
            if (strcmp(all_contacts[i].number, number) == 0) {
                strncpy(current_call_name, all_contacts[i].name, sizeof(current_call_name) - 1);
                break;
            }
        }
        
        set_call_state(CALL_OUTGOING);
        update_ui();
    } else {
        // HFP başarısız, panoya kopyala
        GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(clipboard, number, -1);
        
        snprintf(msg, sizeof(msg), "📋 %s panoya kopyalandı", number);
        log_msg(msg);
        
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_OK,
            "📞 %s", number);
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dialog),
            "HFP bağlantısı kurulamadı.\nNumara panoya kopyalandı.");
        gtk_window_set_title(GTK_WINDOW(dialog), "Arama");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }
}

// Son görüşmelerde satıra çift tıklandığında
static void on_recent_row_activated(GtkTreeView *tree_view, GtkTreePath *path,
                                    GtkTreeViewColumn *column, gpointer data) {
    (void)column; (void)data;
    
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;
    
    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gchar *number = NULL;
        gtk_tree_model_get(model, &iter, 2, &number, -1);  // Sütun 2: Numara
        
        if (number && *number) {
            dial_number(number);
            g_free(number);
        }
    }
}

// Sağ tık menüsünden numarayı kopyala
static void on_copy_number_activate(GtkMenuItem *menuitem, gpointer data) {
    (void)menuitem;
    const char *number = (const char *)data;
    if (number && *number) {
        GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(clipboard, number, -1);
        log_msg("📋 Numara kopyalandı");
    }
}

// Rehber sağ tık menüsü
static gboolean on_recents_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    (void)data;
    
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {  // Sağ tık
        GtkTreeView *tree_view = GTK_TREE_VIEW(widget);
        GtkTreePath *path = NULL;
        
        // Tıklanan satırı bul
        if (gtk_tree_view_get_path_at_pos(tree_view, (gint)event->x, (gint)event->y,
                                          &path, NULL, NULL, NULL)) {
            // Satırı seç
            GtkTreeSelection *selection = gtk_tree_view_get_selection(tree_view);
            gtk_tree_selection_select_path(selection, path);
            
            GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
            GtkTreeIter iter;
            
            if (gtk_tree_model_get_iter(model, &iter, path)) {
                static gchar copied_number[64];
                gchar *number = NULL;
                gtk_tree_model_get(model, &iter, 2, &number, -1);  // Sütun 2: Numara
                
                if (number && *number) {
                    strncpy(copied_number, number, sizeof(copied_number) - 1);
                    copied_number[sizeof(copied_number) - 1] = '\0';
                    g_free(number);
                    
                    // Popup menü oluştur
                    GtkWidget *menu = gtk_menu_new();
                    GtkWidget *copy_item = gtk_menu_item_new_with_label("📋 Numarayı Kopyala");
                    g_signal_connect(copy_item, "activate", G_CALLBACK(on_copy_number_activate), copied_number);
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), copy_item);
                    gtk_widget_show_all(menu);
                    
                    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
                }
            }
            gtk_tree_path_free(path);
        }
        return TRUE;  // Event işlendi
    }
    return FALSE;
}

static gboolean on_contacts_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    (void)data;
    
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {  // Sağ tık
        GtkTreeView *tree_view = GTK_TREE_VIEW(widget);
        GtkTreePath *path = NULL;
        
        // Tıklanan satırı bul
        if (gtk_tree_view_get_path_at_pos(tree_view, (gint)event->x, (gint)event->y,
                                          &path, NULL, NULL, NULL)) {
            // Satırı seç
            GtkTreeSelection *selection = gtk_tree_view_get_selection(tree_view);
            gtk_tree_selection_select_path(selection, path);
            
            GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
            GtkTreeIter iter;
            
            if (gtk_tree_model_get_iter(model, &iter, path)) {
                static gchar copied_number[64];
                gchar *number = NULL;
                gtk_tree_model_get(model, &iter, 1, &number, -1);
                
                if (number && *number) {
                    strncpy(copied_number, number, sizeof(copied_number) - 1);
                    copied_number[sizeof(copied_number) - 1] = '\0';
                    g_free(number);
                    
                    // Popup menü oluştur
                    GtkWidget *menu = gtk_menu_new();
                    GtkWidget *copy_item = gtk_menu_item_new_with_label("📋 Numarayı Kopyala");
                    g_signal_connect(copy_item, "activate", G_CALLBACK(on_copy_number_activate), copied_number);
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), copy_item);
                    gtk_widget_show_all(menu);
                    
                    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
                }
            }
            gtk_tree_path_free(path);
        }
        return TRUE;  // Event işlendi
    }
    return FALSE;
}

// Rehberde satıra çift tıklandığında
static void on_contact_row_activated(GtkTreeView *tree_view, GtkTreePath *path,
                                     GtkTreeViewColumn *column, gpointer data) {
    (void)column; (void)data;
    
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;
    
    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gchar *number = NULL;
        gtk_tree_model_get(model, &iter, 1, &number, -1);  // Sütun 1: Numara
        
        if (number && *number) {
            dial_number(number);
            g_free(number);
        }
    }
}

// ============================================================================
// CALL UI
// ============================================================================

static void update_call_ui(void) {
    char call_text[512];

    switch (current_call_state) {
        case CALL_IDLE:
            snprintf(call_text, sizeof(call_text), "📞 Arama yok");
            // Keep above ve urgency hint'i kapat
            if (window) {
                gtk_window_set_keep_above(GTK_WINDOW(window), FALSE);
                gtk_window_set_urgency_hint(GTK_WINDOW(window), FALSE);
            }
            break;
        case CALL_RINGING:
            if (current_call_name[0] && current_call_number[0]) {
                snprintf(call_text, sizeof(call_text), 
                    "🔔 GELEN ARAMA\n\n%s\n%s",
                    current_call_name, current_call_number);
            } else if (current_call_number[0]) {
                snprintf(call_text, sizeof(call_text), 
                    "🔔 GELEN ARAMA\n\n%s",
                    current_call_number);
            } else {
                snprintf(call_text, sizeof(call_text), "🔔 GELEN ARAMA");
            }
            break;
        case CALL_OUTGOING:
            if (current_call_name[0]) {
                snprintf(call_text, sizeof(call_text), 
                    "📱 Aranıyor...\n\n%s\n%s",
                    current_call_name, current_call_number);
            } else {
                snprintf(call_text, sizeof(call_text), 
                    "📱 Aranıyor...\n\n%s",
                    current_call_number);
            }
            break;
        case CALL_ACTIVE:
            if (current_call_name[0]) {
                snprintf(call_text, sizeof(call_text), 
                    "✅ Görüşme Aktif\n\n%s\n%s",
                    current_call_name, current_call_number);
            } else {
                snprintf(call_text, sizeof(call_text), 
                    "✅ Görüşme Aktif\n\n%s",
                    current_call_number);
            }
            break;
    }

    gtk_label_set_markup(GTK_LABEL(call_status_label), call_text);

    // Gelen arama: Cevapla/Reddet aktif
    // Giden arama: Bitir aktif
    // Aktif görüşme: Bitir aktif
    gtk_widget_set_sensitive(answer_btn, current_call_state == CALL_RINGING);
    gtk_widget_set_sensitive(reject_btn, current_call_state == CALL_RINGING);
    gtk_widget_set_sensitive(hangup_btn, current_call_state == CALL_ACTIVE || current_call_state == CALL_OUTGOING);
    gtk_widget_set_sensitive(test_call_btn, current_call_state == CALL_IDLE);
}

static void set_call_state(CallState new_state) {
    if (current_call_state == new_state) return;
    current_call_state = new_state;

    if (current_call_state == CALL_RINGING) {
        start_ringtone();
        bring_window_to_front();
    } else {
        stop_ringtone();
    }

    if (current_call_state == CALL_IDLE) {
        stop_sco_audio(NULL);
    }

    update_call_ui();
}

// ============================================================================
// STATE
// ============================================================================

static void set_state(AppState new_state) {
    if (current_state == new_state) return;

    char msg[128];
    snprintf(msg, sizeof(msg), "State: %s -> %s",
             state_names[current_state], state_names[new_state]);
    log_msg(msg);

    AppState old_state = current_state;
    current_state = new_state;
    
    // CONNECTED olduğunda arka planda veri yüklemeyi başlat
    if (new_state == STATE_CONNECTED && old_state != STATE_CONNECTED) {
        // HFP kanalını SDP ile bul (henüz bulunmamışsa)
        if (hfp_channel == 0 && device_addr[0]) {
            uint8_t ch = find_hfp_channel(device_addr);
            if (ch) {
                hfp_channel = ch;
            }
        }
        
        // Gelen arama dinleyiciyi başlat
        start_incoming_call_listener();
        
        // Rehberi arka planda yükle (henüz yüklenmemişse)
        // Son aramalar rehber yüklendikten sonra otomatik başlayacak
        if (!phonebook_loaded && !syncing_contacts) {
            syncing_contacts = TRUE;
            log_msg("📥 Veriler arka planda yükleniyor...");
            g_thread_new("preload_phonebook", load_phonebook_thread, NULL);
        }
    }
    
    // CONNECTED'dan çıkarken tüm bağlantıları temizle
    if (old_state == STATE_CONNECTED && new_state != STATE_CONNECTED) {
        cleanup_connection(NULL, FALSE);
    }
}

static void update_ui(void) {
    char status_text[256];
    char info_text[512];
    const char *css_class = "status-idle";

    switch (current_state) {
        case STATE_IDLE:
            snprintf(status_text, sizeof(status_text), "🔵 Hazır");
            snprintf(info_text, sizeof(info_text),
                     "PC pasif modda.\n\n"
                     "'Başlat' butonuna basınca keşfedilebilir olur.\n"
                     "Telefonunuzdan bu PC'yi bulup bağlanın.");
            css_class = "status-idle";
            break;
        case STATE_DISCOVERABLE:
            snprintf(status_text, sizeof(status_text), "📡 Keşfedilebilir - Telefon bekleniyor");
            snprintf(info_text, sizeof(info_text),
                     "Telefonunuzda Bluetooth taraması yapın\n"
                     "ve bu PC'ye bağlanın.\n\n"
                     "Eşleştirme ve bağlantı otomatik kabul edilir.");
            css_class = "status-discoverable";
            break;
        case STATE_PAIRING:
            snprintf(status_text, sizeof(status_text), "🔗 Eşleştirme yapılıyor");
            snprintf(info_text, sizeof(info_text),
                     "Telefondan eşleştirme isteği geldi.\n"
                     "Otomatik onaylandı.");
            css_class = "status-pairing";
            break;
        case STATE_PAIRED:
            snprintf(status_text, sizeof(status_text), "✓ Eşleştirildi");
            snprintf(info_text, sizeof(info_text),
                     "Cihaz: %s\nAdres: %s\n\n"
                     "Otomatik bağlanma deneniyor...",
                     device_name[0] ? device_name : "Bilinmiyor",
                     device_addr[0] ? device_addr : "-" );
            css_class = "status-paired";
            break;
        case STATE_CONNECTING:
            snprintf(status_text, sizeof(status_text), "🔗 Bağlanıyor");
            snprintf(info_text, sizeof(info_text),
                     "Cihaz: %s\nAdres: %s\n\n"
                     "Bağlantı kuruluyor...",
                     device_name[0] ? device_name : "Bilinmiyor",
                     device_addr[0] ? device_addr : "-" );
            css_class = "status-connecting";
            break;
        case STATE_CONNECTED:
            snprintf(status_text, sizeof(status_text), "✅ BAĞLI");
            snprintf(info_text, sizeof(info_text),
                     "Cihaz: %s\nAdres: %s\n\n"
                     "Telefon bu PC'yi kulaklık olarak kullanıyor.",
                     device_name[0] ? device_name : "Bilinmiyor",
                     device_addr[0] ? device_addr : "-" );
            css_class = "status-connected";
            break;
        case STATE_ERROR:
            snprintf(status_text, sizeof(status_text), "❌ HATA");
            snprintf(info_text, sizeof(info_text), "Hata: %s", error_msg);
            css_class = "status-error";
            break;
    }

    gtk_label_set_text(GTK_LABEL(state_label), status_text);
    gtk_label_set_text(GTK_LABEL(info_label), info_text);

    GtkStyleContext *ctx = gtk_widget_get_style_context(state_label);
    gtk_style_context_remove_class(ctx, "status-idle");
    gtk_style_context_remove_class(ctx, "status-discoverable");
    gtk_style_context_remove_class(ctx, "status-pairing");
    gtk_style_context_remove_class(ctx, "status-paired");
    gtk_style_context_remove_class(ctx, "status-connecting");
    gtk_style_context_remove_class(ctx, "status-connected");
    gtk_style_context_remove_class(ctx, "status-error");
    gtk_style_context_add_class(ctx, css_class);

    gtk_widget_set_sensitive(start_btn, current_state == STATE_IDLE);
    gtk_widget_set_sensitive(stop_btn, current_state == STATE_DISCOVERABLE || current_state == STATE_PAIRED);
    gtk_widget_set_sensitive(disconnect_btn, current_state == STATE_CONNECTED);
    gtk_widget_set_sensitive(contacts_search_entry, current_state == STATE_CONNECTED && !syncing_contacts);
    gtk_widget_set_sensitive(sync_recents_btn, current_state == STATE_CONNECTED && !syncing_recents);

    if (current_state == STATE_DISCOVERABLE || current_state == STATE_PAIRING || current_state == STATE_CONNECTING) {
        gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_widget_show(spinner);
    } else {
        gtk_spinner_stop(GTK_SPINNER(spinner));
        gtk_widget_hide(spinner);
    }

    update_call_ui();
}

// ============================================================================
// ADAPTER
// ============================================================================

static gboolean set_adapter_property(const char *prop, GVariant *value) {
    GError *error = NULL;

    g_dbus_connection_call_sync(
        dbus_conn, "org.bluez", adapter_path,
        "org.freedesktop.DBus.Properties", "Set",
        g_variant_new("(ssv)", "org.bluez.Adapter1", prop, value),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (error) {
        snprintf(error_msg, sizeof(error_msg), "Adapter ayarı hatası (%s): %s", prop, error->message);
        log_msg(error_msg);
        g_error_free(error);
        return FALSE;
    }
    return TRUE;
}

static void make_discoverable(gboolean discoverable) {
    set_adapter_property("Powered", g_variant_new_boolean(TRUE));
    set_adapter_property("Discoverable", g_variant_new_boolean(discoverable));
    set_adapter_property("Pairable", g_variant_new_boolean(discoverable));

    if (discoverable) {
        set_adapter_property("DiscoverableTimeout", g_variant_new_uint32(0));
        set_adapter_property("PairableTimeout", g_variant_new_uint32(0));
        log_msg("✓ PC keşfedilebilir yapıldı");
    } else {
        log_msg("✓ Keşfedilebilirlik kapatıldı");
    }
}

// ============================================================================
// AGENT
// ============================================================================

static GDBusNodeInfo *agent_introspection = NULL;

static const gchar agent_xml[] =
    "<node>"
    "  <interface name='org.bluez.Agent1'>"
    "    <method name='Release'/>"
    "    <method name='RequestPinCode'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='pincode' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPinCode'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='pincode' direction='in'/>"
    "    </method>"
    "    <method name='RequestPasskey'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPasskey'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='in'/>"
    "      <arg type='q' name='entered' direction='in'/>"
    "    </method>"
    "    <method name='RequestConfirmation'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='in'/>"
    "    </method>"
    "    <method name='RequestAuthorization'>"
    "      <arg type='o' name='device' direction='in'/>"
    "    </method>"
    "    <method name='AuthorizeService'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "    </method>"
    "    <method name='Cancel'/>"
    "  </interface>"
    "</node>";

static void agent_method_call(GDBusConnection *conn, const gchar *sender,
                              const gchar *object_path, const gchar *interface_name,
                              const gchar *method_name, GVariant *parameters,
                              GDBusMethodInvocation *invocation, gpointer user_data) {
    (void)conn; (void)sender; (void)object_path; (void)interface_name; (void)user_data;

    if (g_strcmp0(method_name, "Release") == 0) {
        log_msg("Agent: Release");
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "RequestPinCode") == 0) {
        log_msg("Agent: PIN isteniyor (0000)");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "0000"));
        return;
    }

    if (g_strcmp0(method_name, "DisplayPinCode") == 0) {
        log_msg("Agent: PIN gösterimi");
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "RequestPasskey") == 0) {
        log_msg("Agent: Passkey isteniyor (0)");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 0));
        return;
    }

    if (g_strcmp0(method_name, "DisplayPasskey") == 0) {
        log_msg("Agent: Passkey gösterimi");
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "RequestConfirmation") == 0) {
        const gchar *device;
        guint32 passkey;
        g_variant_get(parameters, "(&ou)", &device, &passkey);

        char msg[128];
        snprintf(msg, sizeof(msg), "🔔 Eşleştirme isteği: %06u", passkey);
        log_msg(msg);

        strncpy(device_path, device, sizeof(device_path) - 1);

        if (current_state != STATE_CONNECTED) {
            set_state(STATE_PAIRING);
            g_idle_add((GSourceFunc)update_ui, NULL);
        }

        // Otomatik onay
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "RequestAuthorization") == 0) {
        log_msg("Agent: Yetkilendirme (otomatik kabul)");
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "AuthorizeService") == 0) {
        log_msg("Agent: Servis yetkilendirme (otomatik kabul)");
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "Cancel") == 0) {
        log_msg("Agent: İptal edildi");
        set_state(STATE_DISCOVERABLE);
        g_idle_add((GSourceFunc)update_ui, NULL);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
}

static const GDBusInterfaceVTable agent_vtable = {
    agent_method_call,
    NULL,
    NULL,
    {0}
};

static gboolean register_agent(void) {
    GError *error = NULL;

    agent_introspection = g_dbus_node_info_new_for_xml(agent_xml, &error);
    if (error) {
        snprintf(error_msg, sizeof(error_msg), "Agent XML hatası: %s", error->message);
        g_error_free(error);
        return FALSE;
    }

    agent_registration_id = g_dbus_connection_register_object(
        dbus_conn,
        "/org/bluez/agent",
        agent_introspection->interfaces[0],
        &agent_vtable,
        NULL, NULL, &error);

    if (error) {
        snprintf(error_msg, sizeof(error_msg), "Agent kayıt hatası: %s", error->message);
        g_error_free(error);
        return FALSE;
    }

    GVariant *result = g_dbus_connection_call_sync(
        dbus_conn, "org.bluez", "/org/bluez",
        "org.bluez.AgentManager1", "RegisterAgent",
        g_variant_new("(os)", "/org/bluez/agent", "DisplayYesNo"),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (error) {
        if (!strstr(error->message, "Already Exists")) {
            snprintf(error_msg, sizeof(error_msg), "Agent tanıtma hatası: %s", error->message);
            g_error_free(error);
            return FALSE;
        }
        g_error_free(error);
    } else if (result) {
        g_variant_unref(result);
    }

    result = g_dbus_connection_call_sync(
        dbus_conn, "org.bluez", "/org/bluez",
        "org.bluez.AgentManager1", "RequestDefaultAgent",
        g_variant_new("(o)", "/org/bluez/agent"),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (error) {
        g_error_free(error);
    } else if (result) {
        g_variant_unref(result);
    }

    log_msg("✓ Agent kaydedildi");
    return TRUE;
}

// ============================================================================
// AUTO CONNECT
// ============================================================================

static gboolean connect_complete_cb(gpointer data) {
    gboolean success = GPOINTER_TO_INT(data);
    auto_connect_in_progress = FALSE;

    if (success) {
        set_state(STATE_CONNECTED);
        log_msg("✅ Otomatik bağlantı kuruldu");
    } else {
        set_state(STATE_PAIRED);
        log_msg("⚠️ Otomatik bağlantı başarısız, telefon bağlanabilir");
    }

    update_ui();
    return G_SOURCE_REMOVE;
}

static gpointer connect_thread(gpointer data) {
    (void)data;
    if (device_path[0] == '\0') {
        g_idle_add(connect_complete_cb, GINT_TO_POINTER(FALSE));
        return NULL;
    }

    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        dbus_conn, "org.bluez", device_path,
        "org.bluez.Device1", "Connect",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);

    if (error) {
        g_error_free(error);
        g_idle_add(connect_complete_cb, GINT_TO_POINTER(FALSE));
    } else {
        if (result) g_variant_unref(result);
        g_idle_add(connect_complete_cb, GINT_TO_POINTER(TRUE));
    }

    return NULL;
}

static void try_auto_connect(void) {
    if (auto_connect_in_progress || device_path[0] == '\0') return;

    auto_connect_in_progress = TRUE;
    set_state(STATE_CONNECTING);
    update_ui();
    g_thread_new("auto_connect", connect_thread, NULL);
}

// ============================================================================
// CALL HANDLING (HFP HOOKS)
// ============================================================================

static void clear_call_info(void) {
    memset(current_call_number, 0, sizeof(current_call_number));
    memset(current_call_name, 0, sizeof(current_call_name));
}

static gboolean parse_ciev(const char *buf, int *indicator, int *value) {
    const char *p = strstr(buf, "+CIEV:");
    if (!p) return FALSE;

    p += strlen("+CIEV:");
    while (*p == ' ' || *p == '\t') p++;

    char *end = NULL;
    long ind = strtol(p, &end, 10);
    if (end == p) return FALSE;

    while (*end == ' ' || *end == '\t') end++;
    if (*end != ',') return FALSE;
    end++;
    while (*end == ' ' || *end == '\t') end++;

    char *end2 = NULL;
    long val = strtol(end, &end2, 10);
    if (end2 == end) return FALSE;

    if (indicator) *indicator = (int)ind;
    if (value) *value = (int)val;
    return TRUE;
}

static void stop_sco_audio(const char *reason) {
    gboolean was_running = sco_audio_running || (sco_socket >= 0);

    // Önce flag'i kapat ki thread'ler döngüden çıksın
    sco_audio_running = FALSE;
    
    // Thread'lerin döngüden çıkması için kısa bekle
    usleep(50000);  // 50ms
    
    // Socket'i kapat
    if (sco_socket >= 0) {
        shutdown(sco_socket, SHUT_RDWR);
        close(sco_socket);
        sco_socket = -1;
    }
    
    // Thread'lerin tamamen bitmesini bekle (max 500ms)
    for (int i = 0; i < 10 && (pulse_playback || pulse_capture); i++) {
        usleep(50000);  // 50ms
    }

    if (reason && was_running) {
        // Thread-safe log (arka plan thread'inden çağrılabilir)
        g_idle_add((GSourceFunc)lambda_log, g_strdup(reason));
    }

    shutdown_webrtc_aec();
}

static void clear_device_info(void) {
    device_path[0] = '\0';
    device_addr[0] = '\0';
    device_name[0] = '\0';
    device_paired = FALSE;
    hfp_channel = 0;  // HFP kanalını sıfırla
}

static void cleanup_connection(const char *reason, gboolean clear_device) {
    if (reason) {
        log_msg(reason);
    }

    stop_incoming_call_listener();
    hfp_close();
    stop_sco_audio(NULL);
    clear_call_info();
    set_call_state(CALL_IDLE);

    if (clear_device) {
        clear_device_info();
    }
}

static void handle_incoming_call(const char *number) {
    if (!number || !*number) return;

    strncpy(current_call_number, number, sizeof(current_call_number) - 1);
    const char *name = lookup_contact_name(number);
    current_call_name[0] = '\0';
    if (name && *name) {
        strncpy(current_call_name, name, sizeof(current_call_name) - 1);
    }

    log_msg("📞 Gelen arama algılandı");
    set_call_state(CALL_RINGING);
    update_ui();
}

static void on_answer_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;

    log_msg("✅ Arama cevaplandı");

    if (current_call_state != CALL_RINGING) {
        log_msg("⚠️ Cevapla: Geçersiz durumda");
        return;
    }

    gtk_widget_set_sensitive(answer_btn, FALSE);
    gtk_widget_set_sensitive(reject_btn, FALSE);

    if (hfp_listen_socket >= 0) {
        char cmd[] = "ATA\r";
        if (write(hfp_listen_socket, cmd, strlen(cmd)) > 0) {
            usleep(200000);
            char buf[256] = {0};
            read(hfp_listen_socket, buf, sizeof(buf) - 1);
        }
    }

    // SCO bağlantısı kur
    sco_connect();

    set_call_state(CALL_ACTIVE);
    update_ui();
}

static void on_reject_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    log_msg("❌ Arama reddedildi");

    if (current_call_state != CALL_RINGING && current_call_state != CALL_OUTGOING) {
        log_msg("⚠️ Reddet: Geçersiz durumda");
        return;
    }

    gtk_widget_set_sensitive(reject_btn, FALSE);
    gtk_widget_set_sensitive(hangup_btn, FALSE);
    
    // Gelen arama ise
    if (current_call_state == CALL_RINGING && hfp_listen_socket >= 0) {
        char cmd[] = "AT+CHUP\r";
        if (write(hfp_listen_socket, cmd, strlen(cmd)) > 0) {
            usleep(200000);
            char buf[256] = {0};
            read(hfp_listen_socket, buf, sizeof(buf) - 1);
        }
        set_call_state(CALL_IDLE);
        clear_call_info();
    }
    // Giden arama - listen socket üzerinden yapıldıysa
    else if (current_call_state == CALL_OUTGOING && hfp_listen_socket >= 0) {
        log_msg("📱 Giden arama iptal ediliyor...");
        char cmd[] = "AT+CHUP\r";
        if (write(hfp_listen_socket, cmd, strlen(cmd)) > 0) {
            usleep(200000);
            char buf[256] = {0};
            read(hfp_listen_socket, buf, sizeof(buf) - 1);
            log_msg("📱 AT+CHUP gönderildi");
        }
        // SCO'yu kapat
        sco_audio_running = FALSE;
        if (sco_socket >= 0) {
            shutdown(sco_socket, SHUT_RDWR);
            close(sco_socket);
            sco_socket = -1;
        }
        set_call_state(CALL_IDLE);
        clear_call_info();
    }
    // Giden arama - hfp_socket üzerinden
    else if (current_call_state == CALL_OUTGOING && hfp_socket >= 0) {
        hfp_hangup();
    } else {
        set_call_state(CALL_IDLE);
        clear_call_info();
    }
}

static void on_hangup_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    log_msg("🔚 Görüşme bitirildi");

    if (current_call_state != CALL_OUTGOING && current_call_state != CALL_ACTIVE) {
        log_msg("⚠️ Bitir: Geçersiz durumda");
        return;
    }

    gtk_widget_set_sensitive(reject_btn, FALSE);
    gtk_widget_set_sensitive(hangup_btn, FALSE);

    // SCO'yu arka planda kapat (UI donmasın)
    sco_audio_running = FALSE;
    
    // Gelen arama görüşmesi (listen socket üzerinden)
    if (hfp_listen_socket >= 0) {
        char cmd[] = "AT+CHUP\r";
        write(hfp_listen_socket, cmd, strlen(cmd));
        log_msg("📱 AT+CHUP gönderildi (listen)");
    }
    // Giden arama (hfp_socket üzerinden)
    else if (hfp_socket >= 0) {
        char cmd[] = "AT+CHUP\r";
        write(hfp_socket, cmd, strlen(cmd));
        log_msg("📱 AT+CHUP gönderildi");
    }
    
    // SCO temizliğini arka planda yap
    g_thread_new("hangup_cleanup", (GThreadFunc)stop_sco_audio, "🔊 SCO kapatıldı");
    
    set_call_state(CALL_IDLE);
    clear_call_info();
}

static void on_test_call_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    const char *test_number = "5551234";
    if (contacts_count > 0 && contacts[0].number[0]) {
        test_number = contacts[0].number;
    }
    handle_incoming_call(test_number);
}

// ============================================================================
// SIGNALS
// ============================================================================

static void get_device_info(const char *path) {
    GError *error = NULL;

    GVariant *result = g_dbus_connection_call_sync(
        dbus_conn, "org.bluez", path,
        "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", "org.bluez.Device1"),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (error) {
        g_error_free(error);
        return;
    }

    GVariant *props;
    g_variant_get(result, "(@a{sv})", &props);

    GVariant *addr = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
    if (addr) {
        strncpy(device_addr, g_variant_get_string(addr, NULL), sizeof(device_addr) - 1);
        g_variant_unref(addr);
    }

    GVariant *name = g_variant_lookup_value(props, "Name", G_VARIANT_TYPE_STRING);
    if (name) {
        strncpy(device_name, g_variant_get_string(name, NULL), sizeof(device_name) - 1);
        g_variant_unref(name);
    }

    GVariant *paired = g_variant_lookup_value(props, "Paired", G_VARIANT_TYPE_BOOLEAN);
    if (paired) {
        device_paired = g_variant_get_boolean(paired);
        g_variant_unref(paired);
    }

    strncpy(device_path, path, sizeof(device_path) - 1);

    g_variant_unref(props);
    g_variant_unref(result);
}

static void on_properties_changed(GDBusConnection *conn, const gchar *sender,
                                   const gchar *path, const gchar *interface,
                                   const gchar *signal, GVariant *params,
                                   gpointer user_data) {
    (void)conn; (void)sender; (void)interface; (void)signal; (void)user_data;

    const gchar *iface;
    GVariant *changed_props;

    g_variant_get(params, "(&s@a{sv}as)", &iface, &changed_props, NULL);

    if (g_strcmp0(iface, "org.bluez.Device1") == 0) {
        gboolean is_same_device = (device_path[0] == '\0') || (strcmp(path, device_path) == 0);

        GVariant *paired = g_variant_lookup_value(changed_props, "Paired", G_VARIANT_TYPE_BOOLEAN);
        if (paired) {
            gboolean is_paired = g_variant_get_boolean(paired);
            device_paired = is_paired;
            if (is_paired && is_same_device) {
                get_device_info(path);
                log_msg("✓ Eşleştirme tamamlandı");
                if (current_state != STATE_CONNECTED) {
                    set_state(STATE_PAIRED);
                    g_idle_add((GSourceFunc)update_ui, NULL);
                    try_auto_connect();
                }
            } else if (!is_paired && is_same_device) {
                log_msg("🔓 Eşleştirme kaldırıldı");
                cleanup_connection("📴 Cihaz ayrıldı", TRUE);
                set_state(current_state == STATE_IDLE ? STATE_IDLE : STATE_DISCOVERABLE);
                g_idle_add((GSourceFunc)update_ui, NULL);
            }
            g_variant_unref(paired);
        }

        GVariant *connected = g_variant_lookup_value(changed_props, "Connected", G_VARIANT_TYPE_BOOLEAN);
        if (connected) {
            gboolean is_connected = g_variant_get_boolean(connected);
            if (is_connected && is_same_device) {
                get_device_info(path);
                log_msg("📱 Cihaz bağlandı");
                auto_connect_in_progress = FALSE;
                set_state(STATE_CONNECTED);
                g_idle_add((GSourceFunc)update_ui, NULL);
            } else if (!is_connected && is_same_device) {
                if (current_state == STATE_CONNECTED || current_state == STATE_CONNECTING) {
                    cleanup_connection("📴 Bağlantı kesildi", FALSE);
                    set_state(device_paired ? STATE_PAIRED : STATE_DISCOVERABLE);
                    g_idle_add((GSourceFunc)update_ui, NULL);
                }
            }
            g_variant_unref(connected);
        }
    }

    g_variant_unref(changed_props);
}

static void on_interfaces_removed(GDBusConnection *conn, const gchar *sender,
                                  const gchar *path, const gchar *interface,
                                  const gchar *signal, GVariant *params,
                                  gpointer user_data) {
    (void)conn; (void)sender; (void)path; (void)interface; (void)signal; (void)user_data;

    const gchar *obj_path = NULL;
    GVariant *ifaces = NULL;
    g_variant_get(params, "(&o@as)", &obj_path, &ifaces);

    if (obj_path && device_path[0] && strcmp(obj_path, device_path) == 0) {
        GVariantIter iter;
        const gchar *iface_name = NULL;
        gboolean removed = FALSE;

        g_variant_iter_init(&iter, ifaces);
        while (g_variant_iter_next(&iter, "&s", &iface_name)) {
            if (g_strcmp0(iface_name, "org.bluez.Device1") == 0) {
                removed = TRUE;
                break;
            }
        }

        if (removed) {
            cleanup_connection("🗑️ Cihaz kaldırıldı", TRUE);
            set_state(current_state == STATE_IDLE ? STATE_IDLE : STATE_DISCOVERABLE);
            g_idle_add((GSourceFunc)update_ui, NULL);
        }
    }

    if (ifaces) {
        g_variant_unref(ifaces);
    }
}

static void setup_dbus_signals(void) {
    g_dbus_connection_signal_subscribe(
        dbus_conn,
        "org.bluez",
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_properties_changed,
        NULL, NULL);

    g_dbus_connection_signal_subscribe(
        dbus_conn,
        "org.bluez",
        "org.freedesktop.DBus.ObjectManager",
        "InterfacesRemoved",
        NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_interfaces_removed,
        NULL, NULL);

    log_msg("✓ D-Bus sinyal dinleyici kuruldu");
}

// ============================================================================
// INITIAL STATE SYNC
// ============================================================================

static void sync_initial_state(void) {
    GError *error = NULL;
    gboolean found_paired = FALSE;
    char paired_path[256] = {0};

    GVariant *result = g_dbus_connection_call_sync(
        dbus_conn, "org.bluez", "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
        NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (error) {
        g_error_free(error);
        return;
    }

    GVariant *objects;
    g_variant_get(result, "(@a{oa{sa{sv}}})", &objects);

    GVariantIter iter;
    const gchar *path;
    GVariant *ifaces;
    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *device_iface = g_variant_lookup_value(ifaces, "org.bluez.Device1", NULL);
        if (device_iface) {
            GVariant *connected = g_variant_lookup_value(device_iface, "Connected", G_VARIANT_TYPE_BOOLEAN);
            GVariant *paired = g_variant_lookup_value(device_iface, "Paired", G_VARIANT_TYPE_BOOLEAN);

            if (connected && g_variant_get_boolean(connected)) {
                get_device_info(path);
                log_msg("ℹ️ Başlangıçta bağlı cihaz bulundu");
                set_state(STATE_CONNECTED);
                g_variant_unref(connected);
                if (paired) g_variant_unref(paired);
                g_variant_unref(device_iface);
                g_variant_unref(ifaces);
                g_variant_unref(objects);
                g_variant_unref(result);
                return;
            }

            if (paired && g_variant_get_boolean(paired) && !found_paired) {
                strncpy(paired_path, path, sizeof(paired_path) - 1);
                found_paired = TRUE;
            }

            if (connected) g_variant_unref(connected);
            if (paired) g_variant_unref(paired);
            g_variant_unref(device_iface);
        }
        g_variant_unref(ifaces);
    }

    if (found_paired) {
        get_device_info(paired_path);
        log_msg("ℹ️ Başlangıçta eşleşmiş cihaz bulundu");
        set_state(STATE_PAIRED);
    }

    g_variant_unref(objects);
    g_variant_unref(result);
}

// ============================================================================
// DIALPAD CALLBACKS
// ============================================================================

static void on_dialpad_clicked(GtkWidget *widget, gpointer data) {
    const char *key = (const char *)data;
    GtkEntry *entry = GTK_ENTRY(g_object_get_data(G_OBJECT(widget), "entry"));
    if (entry) {
        const gchar *text = gtk_entry_get_text(entry);
        gchar *new_text = g_strdup_printf("%s%s", text, key);
        gtk_entry_set_text(entry, new_text);
        g_free(new_text);
    }
}

static void on_dial_call_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    GtkEntry *entry = GTK_ENTRY(data);
    if (!entry) return;
    
    const gchar *number = gtk_entry_get_text(entry);
    if (!number || !*number) {
        log_msg("⚠️ Numara girilmedi");
        return;
    }
    
    dial_number(number);
    
    // Entry'yi temizle
    gtk_entry_set_text(entry, "");
}

// ============================================================================
// BUTTONS
// ============================================================================

static void on_start_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;

    log_msg("🚀 Başlatılıyor...");

    if (!register_agent()) {
        set_state(STATE_ERROR);
        update_ui();
        return;
    }

    make_discoverable(TRUE);
    set_state(STATE_DISCOVERABLE);
    update_ui();
}

static void on_stop_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;

    make_discoverable(FALSE);
    set_state(STATE_IDLE);
    update_ui();
}

static void on_disconnect_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;

    if (device_path[0]) {
        GError *error = NULL;
        g_dbus_connection_call_sync(
            dbus_conn, "org.bluez", device_path,
            "org.bluez.Device1", "Disconnect",
            NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

        if (error) {
            log_msg(error->message);
            g_error_free(error);
        } else {
            log_msg("🔌 Bağlantı kesildi");
        }
    }

    cleanup_connection(NULL, FALSE);
    set_state(STATE_DISCOVERABLE);
    update_ui();
}

// ============================================================================
// DBUS
// ============================================================================

static gboolean init_dbus(void) {
    GError *error = NULL;

    dbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (error) {
        snprintf(error_msg, sizeof(error_msg), "D-Bus hatası: %s", error->message);
        g_error_free(error);
        return FALSE;
    }

    log_msg("✓ D-Bus bağlantısı kuruldu");
    return TRUE;
}

// ============================================================================
// UI
// ============================================================================

static void apply_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();

    const char *css =
        /* Global - HER ŞEY koyu */
        "* { background-color: #161b22; color: #c9d1d9; }"
        "window { background: #0d1117; }"
        "box { background: transparent; }"
        "grid { background: transparent; }"
        
        ".title { font-size: 18px; font-weight: bold; color: #58a6ff; }"
        ".status-idle { color: #8b949e; }"
        ".status-discoverable { color: #58a6ff; font-weight: bold; }"
        ".status-pairing { color: #d29922; font-weight: bold; }"
        ".status-paired { color: #3fb950; }"
        ".status-connecting { color: #a371f7; font-weight: bold; }"
        ".status-connected { color: #3fb950; font-weight: bold; }"
        ".status-error { color: #f85149; font-weight: bold; }"
        
        /* Labels - tüm yazılar açık renk */
        "label { color: #c9d1d9; background: transparent; }"
        ".info-label { color: #8b949e; font-size: 12px; }"
        ".call-label { color: #ffffff; font-size: 14px; }"
        ".call-ringing { color: #ffa657; font-weight: bold; font-size: 16px; }"
        
        /* Notebook tabs */
        "notebook { background: #0d1117; }"
        "notebook header { background: #21262d; }"
        "notebook header tabs { background: #21262d; }"
        "notebook stack { background: #0d1117; }"
        "notebook tab { padding: 8px 16px; background: #21262d; color: #8b949e; border: none; }"
        "notebook tab:checked { background: #0d1117; color: #58a6ff; border-bottom: 2px solid #58a6ff; }"
        "notebook tab:hover { color: #c9d1d9; }"
        
        /* Scrolled windows & viewport */
        "scrolledwindow { background: #0d1117; }"
        "scrolledwindow > viewport { background: #0d1117; }"
        "viewport { background: #0d1117; }"
        
        /* List views */
        ".list-view { background: #0d1117; color: #c9d1d9; }"
        ".list-view:selected { background: #238636; color: #ffffff; }"
        "treeview { background: #0d1117; color: #c9d1d9; }"
        "treeview:selected { background: #238636; color: #ffffff; }"
        "treeview header { background: #21262d; }"
        "treeview header button { background: #21262d; color: #8b949e; border: none; padding: 8px; }"
        
        /* Buttons */
        "button { background: #21262d; color: #c9d1d9; border: 1px solid #30363d; padding: 8px 14px; border-radius: 6px; }"
        "button:hover { background: #30363d; border-color: #8b949e; }"
        "button:disabled { background: #161b22; color: #484f58; border-color: #21262d; }"
        ".btn-start { background: #238636; border-color: #2ea043; color: white; }"
        ".btn-start:hover { background: #2ea043; }"
        ".btn-stop { background: #da3633; border-color: #f85149; color: white; }"
        ".btn-stop:hover { background: #f85149; }"
        ".btn-answer { background: #238636; border-color: #2ea043; color: white; font-weight: bold; }"
        ".btn-answer:hover { background: #2ea043; }"
        ".btn-reject { background: #da3633; border-color: #f85149; color: white; font-weight: bold; }"
        ".btn-reject:hover { background: #f85149; }"
        ".sync-btn { background: #1f6feb; border-color: #388bfd; color: white; padding: 6px 12px; }"
        ".sync-btn:hover { background: #388bfd; }"
        
        /* Dialpad */
        ".dialpad-btn { font-size: 20px; font-weight: bold; background: #21262d; color: #c9d1d9; border-radius: 50%; }"
        ".dialpad-btn:hover { background: #30363d; }"
        ".dial-entry { font-size: 24px; background: #0d1117; color: #c9d1d9; border: 1px solid #30363d; border-radius: 6px; }"
        
        /* Search entry */
        "entry { background: #0d1117; color: #c9d1d9; border: 1px solid #30363d; border-radius: 6px; padding: 8px; }"
        "entry:focus { border-color: #58a6ff; }"
        
        /* Log view */
        ".log-view { background: #0d1117; color: #8b949e; font-family: monospace; font-size: 11px; padding: 8px; }"
        "textview { background: #0d1117; color: #c9d1d9; }"
        "textview text { background: #0d1117; color: #c9d1d9; }";

    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void create_ui(void) {
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Bluetooth Kulaklık Simülatörü");
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 650);
    gtk_container_set_border_width(GTK_CONTAINER(window), 12);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);

    // Üst bilgi alanı (kompakt)
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(main_vbox), header_box, FALSE, FALSE, 0);

    GtkWidget *title = gtk_label_new("🎧 BT Kulaklık");
    GtkStyleContext *ctx = gtk_widget_get_style_context(title);
    gtk_style_context_add_class(ctx, "title");
    gtk_box_pack_start(GTK_BOX(header_box), title, FALSE, FALSE, 0);

    state_label = gtk_label_new("⚪ Hazır");
    gtk_widget_set_halign(state_label, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(header_box), state_label, FALSE, FALSE, 0);

    spinner = gtk_spinner_new();
    gtk_box_pack_end(GTK_BOX(header_box), spinner, FALSE, FALSE, 0);

    // Kontrol butonları (kompakt)
    GtkWidget *ctrl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(main_vbox), ctrl_box, FALSE, FALSE, 0);

    start_btn = gtk_button_new_with_label("▶ Başlat");
    ctx = gtk_widget_get_style_context(start_btn);
    gtk_style_context_add_class(ctx, "btn-start");
    g_signal_connect(start_btn, "clicked", G_CALLBACK(on_start_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(ctrl_box), start_btn, TRUE, TRUE, 0);

    stop_btn = gtk_button_new_with_label("⏹ Durdur");
    ctx = gtk_widget_get_style_context(stop_btn);
    gtk_style_context_add_class(ctx, "btn-stop");
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(on_stop_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(ctrl_box), stop_btn, TRUE, TRUE, 0);

    disconnect_btn = gtk_button_new_with_label("🔌 Kes");
    g_signal_connect(disconnect_btn, "clicked", G_CALLBACK(on_disconnect_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(ctrl_box), disconnect_btn, TRUE, TRUE, 0);

    // Arama durumu
    info_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(info_label), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(info_label), "info-label");
    gtk_box_pack_start(GTK_BOX(main_vbox), info_label, FALSE, FALSE, 0);

    call_status_label = gtk_label_new("📞 Arama yok");
    gtk_label_set_xalign(GTK_LABEL(call_status_label), 0.5);  // Ortala
    gtk_label_set_use_markup(GTK_LABEL(call_status_label), TRUE);
    gtk_label_set_justify(GTK_LABEL(call_status_label), GTK_JUSTIFY_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(call_status_label), "call-label");
    gtk_box_pack_start(GTK_BOX(main_vbox), call_status_label, FALSE, FALSE, 4);

    // Arama butonları
    GtkWidget *call_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(main_vbox), call_btn_box, FALSE, FALSE, 0);

    answer_btn = gtk_button_new_with_label("✅ Cevapla");
    gtk_style_context_add_class(gtk_widget_get_style_context(answer_btn), "btn-answer");
    g_signal_connect(answer_btn, "clicked", G_CALLBACK(on_answer_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(call_btn_box), answer_btn, TRUE, TRUE, 0);

    reject_btn = gtk_button_new_with_label("❌ Reddet");
    gtk_style_context_add_class(gtk_widget_get_style_context(reject_btn), "btn-reject");
    g_signal_connect(reject_btn, "clicked", G_CALLBACK(on_reject_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(call_btn_box), reject_btn, TRUE, TRUE, 0);

    hangup_btn = gtk_button_new_with_label("🔚 Bitir");
    gtk_style_context_add_class(gtk_widget_get_style_context(hangup_btn), "btn-reject");
    g_signal_connect(hangup_btn, "clicked", G_CALLBACK(on_hangup_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(call_btn_box), hangup_btn, TRUE, TRUE, 0);

    test_call_btn = gtk_button_new_with_label("📞 Test");
    g_signal_connect(test_call_btn, "clicked", G_CALLBACK(on_test_call_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(call_btn_box), test_call_btn, TRUE, TRUE, 0);

    // ========== TAB YAPISI ==========
    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
    gtk_box_pack_start(GTK_BOX(main_vbox), notebook, TRUE, TRUE, 0);

    // ----- TAB 1: Tuş Takımı -----
    GtkWidget *dialpad_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(dialpad_page), 8);
    
    GtkWidget *dial_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(dial_entry), "Numara girin...");
    gtk_entry_set_alignment(GTK_ENTRY(dial_entry), 0.5);
    gtk_widget_set_size_request(dial_entry, -1, 40);
    ctx = gtk_widget_get_style_context(dial_entry);
    gtk_style_context_add_class(ctx, "dial-entry");
    gtk_box_pack_start(GTK_BOX(dialpad_page), dial_entry, FALSE, FALSE, 0);

    // Tuş takımı grid
    GtkWidget *dialpad_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(dialpad_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(dialpad_grid), 6);
    gtk_widget_set_halign(dialpad_grid, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(dialpad_page), dialpad_grid, TRUE, FALSE, 0);

    const char *keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"};
    for (int i = 0; i < 12; i++) {
        GtkWidget *btn = gtk_button_new_with_label(keys[i]);
        gtk_widget_set_size_request(btn, 70, 50);
        ctx = gtk_widget_get_style_context(btn);
        gtk_style_context_add_class(ctx, "dialpad-btn");
        g_object_set_data(G_OBJECT(btn), "entry", dial_entry);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_dialpad_clicked), (gpointer)keys[i]);
        gtk_grid_attach(GTK_GRID(dialpad_grid), btn, i % 3, i / 3, 1, 1);
    }

    GtkWidget *call_btn = gtk_button_new_with_label("📞 Ara");
    gtk_widget_set_size_request(call_btn, 220, 50);
    ctx = gtk_widget_get_style_context(call_btn);
    gtk_style_context_add_class(ctx, "btn-start");
    g_object_set_data(G_OBJECT(call_btn), "entry", dial_entry);
    g_signal_connect(call_btn, "clicked", G_CALLBACK(on_dial_call_clicked), dial_entry);
    gtk_box_pack_start(GTK_BOX(dialpad_page), call_btn, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), dialpad_page, 
                             gtk_label_new("📱 Tuş Takımı"));

    // ----- TAB 2: Son Görüşmeler -----
    GtkWidget *recents_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(recents_page), 8);

    GtkWidget *recents_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(recents_page), recents_header, FALSE, FALSE, 0);

    GtkWidget *recents_title = gtk_label_new("🕘 Son Görüşmeler");
    gtk_widget_set_halign(recents_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(recents_header), recents_title, TRUE, TRUE, 0);

    sync_recents_btn = gtk_button_new_with_label("🔄 Eşitle");
    ctx = gtk_widget_get_style_context(sync_recents_btn);
    gtk_style_context_add_class(ctx, "sync-btn");
    g_signal_connect(sync_recents_btn, "clicked", G_CALLBACK(on_sync_recents_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(recents_header), sync_recents_btn, FALSE, FALSE, 0);
    
    recents_spinner = gtk_spinner_new();
    gtk_box_pack_end(GTK_BOX(recents_header), recents_spinner, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(recents_spinner, TRUE);

    GtkWidget *recent_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(recent_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(recents_page), recent_scroll, TRUE, TRUE, 0);

    recent_store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    recent_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(recent_store));

    GtkCellRenderer *recent_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *type_col = gtk_tree_view_column_new_with_attributes(
        "Tür", recent_renderer, "text", 0, NULL);
    gtk_tree_view_column_set_min_width(type_col, 60);
    gtk_tree_view_column_set_resizable(type_col, TRUE);
    gtk_tree_view_column_set_fixed_width(type_col, col_recent_type);
    g_signal_connect(type_col, "notify::width", G_CALLBACK(on_col_recent_type_width), NULL);
    GtkTreeViewColumn *rname_col = gtk_tree_view_column_new_with_attributes(
        "Ad", recent_renderer, "text", 1, NULL);
    gtk_tree_view_column_set_resizable(rname_col, TRUE);
    gtk_tree_view_column_set_fixed_width(rname_col, col_recent_name);
    g_signal_connect(rname_col, "notify::width", G_CALLBACK(on_col_recent_name_width), NULL);
    GtkTreeViewColumn *rnum_col = gtk_tree_view_column_new_with_attributes(
        "Numara", recent_renderer, "text", 2, NULL);
    gtk_tree_view_column_set_resizable(rnum_col, TRUE);
    gtk_tree_view_column_set_fixed_width(rnum_col, col_recent_number);
    g_signal_connect(rnum_col, "notify::width", G_CALLBACK(on_col_recent_number_width), NULL);
    GtkTreeViewColumn *rtime_col = gtk_tree_view_column_new_with_attributes(
        "Zaman", recent_renderer, "text", 3, NULL);
    gtk_tree_view_column_set_resizable(rtime_col, TRUE);
    gtk_tree_view_column_set_fixed_width(rtime_col, col_recent_time);
    g_signal_connect(rtime_col, "notify::width", G_CALLBACK(on_col_recent_time_width), NULL);

    gtk_tree_view_append_column(GTK_TREE_VIEW(recent_view), type_col);
    gtk_tree_view_append_column(GTK_TREE_VIEW(recent_view), rname_col);
    gtk_tree_view_append_column(GTK_TREE_VIEW(recent_view), rnum_col);
    gtk_tree_view_append_column(GTK_TREE_VIEW(recent_view), rtime_col);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(recent_view), TRUE);

    ctx = gtk_widget_get_style_context(recent_view);
    gtk_style_context_add_class(ctx, "list-view");
    
    // Çift tıklama ile arama
    g_signal_connect(recent_view, "row-activated", G_CALLBACK(on_recent_row_activated), NULL);
    // Sağ tıklama ile menü
    g_signal_connect(recent_view, "button-press-event", G_CALLBACK(on_recents_button_press), NULL);

    gtk_container_add(GTK_CONTAINER(recent_scroll), recent_view);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), recents_page, 
                             gtk_label_new("🕘 Son Aramalar"));

    // ----- TAB 3: Rehber -----
    GtkWidget *contacts_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(contacts_page), 8);

    GtkWidget *contacts_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(contacts_page), contacts_header, FALSE, FALSE, 0);

    GtkWidget *contacts_title = gtk_label_new("👥 Rehber");
    gtk_widget_set_halign(contacts_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(contacts_header), contacts_title, TRUE, TRUE, 0);
    
    // Yenile butonu
    GtkWidget *refresh_btn = gtk_button_new_with_label("🔄 Yenile");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_phonebook_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(contacts_header), refresh_btn, FALSE, FALSE, 0);
    
    contacts_spinner = gtk_spinner_new();
    gtk_box_pack_end(GTK_BOX(contacts_header), contacts_spinner, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(contacts_spinner, TRUE);

    // Arama kutusu + arama kutusu altında bilgi etiketi
    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(contacts_page), search_box, FALSE, FALSE, 0);
    
    contacts_search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(contacts_search_entry), "İsim veya numara ara... (min 2 karakter)");
    g_signal_connect(contacts_search_entry, "search-changed", G_CALLBACK(on_contacts_search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(search_box), contacts_search_entry, TRUE, TRUE, 0);

    GtkWidget *contacts_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(contacts_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(contacts_page), contacts_scroll, TRUE, TRUE, 0);

    contacts_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    contacts_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(contacts_store));

    GtkCellRenderer *contacts_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *name_col = gtk_tree_view_column_new_with_attributes(
        "Ad", contacts_renderer, "text", 0, NULL);
    gtk_tree_view_column_set_resizable(name_col, TRUE);
    gtk_tree_view_column_set_fixed_width(name_col, col_contacts_name);
    g_signal_connect(name_col, "notify::width", G_CALLBACK(on_col_contacts_name_width), NULL);
    GtkTreeViewColumn *num_col = gtk_tree_view_column_new_with_attributes(
        "Numara", contacts_renderer, "text", 1, NULL);
    gtk_tree_view_column_set_resizable(num_col, TRUE);
    gtk_tree_view_column_set_fixed_width(num_col, col_contacts_number);
    g_signal_connect(num_col, "notify::width", G_CALLBACK(on_col_contacts_number_width), NULL);

    gtk_tree_view_append_column(GTK_TREE_VIEW(contacts_view), name_col);
    gtk_tree_view_append_column(GTK_TREE_VIEW(contacts_view), num_col);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(contacts_view), TRUE);

    ctx = gtk_widget_get_style_context(contacts_view);
    gtk_style_context_add_class(ctx, "list-view");
    
    // Çift tıklama ile arama
    g_signal_connect(contacts_view, "row-activated", G_CALLBACK(on_contact_row_activated), NULL);
    // Sağ tıklama ile menü
    g_signal_connect(contacts_view, "button-press-event", G_CALLBACK(on_contacts_button_press), NULL);

    gtk_container_add(GTK_CONTAINER(contacts_scroll), contacts_view);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), contacts_page, 
                             gtk_label_new("👥 Rehber"));

    // ----- TAB 4: Log -----
    GtkWidget *log_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(log_page), 8);

    GtkWidget *log_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(log_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(log_page), log_scroll, TRUE, TRUE, 0);

    log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_view), GTK_WRAP_WORD);
    log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));

    ctx = gtk_widget_get_style_context(log_view);
    gtk_style_context_add_class(ctx, "log-view");

    gtk_container_add(GTK_CONTAINER(log_scroll), log_view);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), log_page, 
                             gtk_label_new("📋 Log"));
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    load_settings();  // Sütun genişliklerini yükle
    apply_css();
    create_ui();

    // CSV'den hızlı yükleme
    if (load_contacts_from_csv()) {
        phonebook_loaded = TRUE;
        // all_contacts'tan contacts'a kopyala (arama için)
        contacts_count = 0;
        for (int i = 0; i < all_contacts_count && contacts_count < 200; i++) {
            strncpy(contacts[contacts_count].name, all_contacts[i].name, 127);
            strncpy(contacts[contacts_count].number, all_contacts[i].number, 63);
            contacts_count++;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "📂 CSV'den %d kişi yüklendi", all_contacts_count);
        log_msg(msg);
    }
    if (load_recents_from_csv()) {
        char msg[64];
        snprintf(msg, sizeof(msg), "📂 CSV'den %d arama kaydı yüklendi", recent_count);
        log_msg(msg);
    }
    
    refresh_contacts_view();
    refresh_recents_view();

    if (!init_dbus()) {
        set_state(STATE_ERROR);
    } else {
        setup_dbus_signals();
        set_state(STATE_IDLE);
        sync_initial_state();
    }

    update_ui();
    gtk_widget_show_all(window);
    gtk_widget_hide(spinner);

    log_msg("🎧 Bluetooth Kulaklık Simülatörü (Pasif Mod)");
    log_msg("ℹ️ 'Başlat' butonuna basın, telefon bağlansın");

    gtk_main();

    make_discoverable(FALSE);
    return 0;
}
