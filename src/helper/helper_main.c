/*
 * helper_main.c - 98Modem helper GUI application with live listener settings
 * and a persistent phonebook.
 */

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string.h>

#include "ipc_shared.h"
#include "ipc_helper.h"
#include "net_common.h"
#include "net_winsock.h"
#include "session_log.h"

#ifndef NOTIFYICONDATA_V1_SIZE
#define NOTIFYICONDATA_V1_SIZE FIELD_OFFSET(NOTIFYICONDATAA, szTip[64])
#endif

#define HELPER_APP_NAME               "98Modem Helper"
#define HELPER_APP_MAIN_CLASS         "VMODEMHelperMainWindow"
#define HELPER_APP_ENTRY_CLASS        "VMODEMHelperEntryWindow"
#define HELPER_APP_TIMER_ID           1U
#define HELPER_APP_TIMER_MS           10U
#define HELPER_APP_TRAY_ID            1U
#define HELPER_APP_WM_TRAY            (WM_USER + 0x71)

#define HELPER_CTRL_LISTENER_ENABLED  1001
#define HELPER_CTRL_LISTENER_HOST     1002
#define HELPER_CTRL_LISTENER_PORT     1003
#define HELPER_CTRL_LISTENER_APPLY    1004
#define HELPER_CTRL_PHONEBOOK_LIST    1005
#define HELPER_CTRL_PHONEBOOK_ADD     1006
#define HELPER_CTRL_PHONEBOOK_EDIT    1007
#define HELPER_CTRL_PHONEBOOK_DELETE  1008
#define HELPER_CTRL_STATUS_LISTENER   1009
#define HELPER_CTRL_STATUS_RUNTIME    1010
#define HELPER_CTRL_LOGGING_ENABLED   1011
#define HELPER_CTRL_STATUS_LOGGING    1012

#define HELPER_MENU_TRAY_OPEN         2001
#define HELPER_MENU_TRAY_EXIT         2002

#define HELPER_DIALOG_EDIT_NAME       3001
#define HELPER_DIALOG_EDIT_NUMBER     3002
#define HELPER_DIALOG_EDIT_TARGET     3003
#define HELPER_DIALOG_CHECK_RAW       3004

#define HELPER_ENTRY_EDIT_ADD         (-1)

typedef struct HELPER_APP HELPER_APP;

typedef struct HELPER_ENTRY_DIALOG_STATE {
    HELPER_APP          *app;
    HWND                hwnd;
    HWND                hwnd_name;
    HWND                hwnd_number;
    HWND                hwnd_target;
    HWND                hwnd_raw_mode;
    HELPER_PHONEBOOK_ENTRY result;
    char                title[64];
    int                 number_locked;
    int                 done;
    int                 accepted;
} HELPER_ENTRY_DIALOG_STATE;

struct HELPER_APP {
    HINSTANCE           hInstance;
    HANDLE              hDevice;
    unsigned long       helper_generation;
    char                config_path[MAX_PATH];
    char                log_path[MAX_PATH];
    HELPER_APP_CONFIG   config;
    HELPER_SESSION_LOG  session_log;
    HELPER_NET_RUNTIME  net;
    VMODEM_PROTOCOL_MESSAGE deferred_message;
    int                 deferred_valid;
    VMODEM_PROTOCOL_MESSAGE pending_connect_message;
    int                 pending_connect_valid;
    HWND                hwnd_main;
    HWND                hwnd_listener_enabled;
    HWND                hwnd_listener_host;
    HWND                hwnd_listener_port;
    HWND                hwnd_logging_enabled;
    HWND                hwnd_phonebook_list;
    HWND                hwnd_listener_status;
    HWND                hwnd_logging_status;
    HWND                hwnd_runtime_status;
    int                 tray_added;
    int                 exiting;
    int                 suppress_listener_updates;
    int                 vxd_trace_poll_state;
    DWORD               last_trace_poll_error;
    HELPER_ENTRY_DIALOG_STATE *active_dialog;
};

static HELPER_APP *g_helper_app = NULL;
static int g_helper_main_class_registered = 0;
static int g_helper_entry_class_registered = 0;

static int helper_build_module_file_path(char *path,
                                         unsigned int capacity,
                                         const char *file_name);
static int helper_build_config_path(char *path, unsigned int capacity);
static int helper_build_log_path(char *path, unsigned int capacity);
static void helper_show_error(HWND owner, const char *title, DWORD detail);
static void helper_show_message(HWND owner,
                                const char *title,
                                const char *message,
                                UINT flags);
static int helper_register_window_classes(HINSTANCE hInstance);
static void helper_app_sync_listener_controls(HELPER_APP *app);
static void helper_app_refresh_phonebook_list(HELPER_APP *app);
static void helper_app_update_status(HELPER_APP *app);
static int helper_app_retry_pending_dial(HELPER_APP *app);
static void helper_app_clear_pending_connect(HELPER_APP *app, int close_dialog);
static int helper_app_process_message(HELPER_APP *app,
                                      const VMODEM_PROTOCOL_MESSAGE *message,
                                      int isDeferred,
                                      int *pStillDeferred);
static int helper_app_retry_deferred(HELPER_APP *app);
static int helper_app_check_runtime_error(HELPER_APP *app);
static int helper_app_drain_ipc(HELPER_APP *app);
static int helper_app_drain_vxd_trace(HELPER_APP *app);
static int helper_app_prompt_unknown_number(HELPER_APP *app);
static int helper_app_edit_phonebook_entry(HELPER_APP *app,
                                           int entry_index,
                                           const HELPER_PHONEBOOK_ENTRY *initial_entry,
                                           int lock_number,
                                           const char *title,
                                           int retry_pending_after_save);
static int helper_app_commit_phonebook_entry(HELPER_APP *app,
                                             int entry_index,
                                             const HELPER_PHONEBOOK_ENTRY *entry);
static int helper_entry_dialog_run(HELPER_APP *app,
                                   const char *title,
                                   const HELPER_PHONEBOOK_ENTRY *initial_entry,
                                   int lock_number,
                                   HELPER_PHONEBOOK_ENTRY *result);
static int helper_app_apply_listener_from_controls(HELPER_APP *app);
static void helper_app_restore_window(HELPER_APP *app);
static int helper_app_hide_to_tray(HELPER_APP *app);
static void helper_app_remove_tray_icon(HELPER_APP *app);
static int helper_app_initialize(HELPER_APP *app, HINSTANCE hInstance);
static void helper_app_shutdown(HELPER_APP *app);
static void helper_app_request_exit(HELPER_APP *app);
static int helper_app_get_selected_phonebook_index(HELPER_APP *app);
static void helper_app_select_phonebook_index(HELPER_APP *app, int index);
static void helper_app_log_event(HELPER_APP *app,
                                 const char *event_name,
                                 unsigned long session_id,
                                 const char *detail);
static void helper_app_log_protocol(HELPER_APP *app,
                                    const char *direction,
                                    const VMODEM_PROTOCOL_MESSAGE *message);
static void helper_app_log_vxd_trace_line(HELPER_APP *app,
                                          const char *text);
static LRESULT CALLBACK helper_main_window_proc(HWND hwnd,
                                                UINT message,
                                                WPARAM wParam,
                                                LPARAM lParam);
static LRESULT CALLBACK helper_entry_window_proc(HWND hwnd,
                                                 UINT message,
                                                 WPARAM wParam,
                                                 LPARAM lParam);

static int helper_build_module_file_path(char *path,
                                         unsigned int capacity,
                                         const char *file_name)
{
    DWORD length;
    unsigned int file_name_length;

    if (path == NULL ||
        file_name == NULL ||
        file_name[0] == '\0' ||
        capacity < 2U) {
        return 0;
    }

    length = GetModuleFileNameA(NULL, path, capacity);
    if (length == 0 || length >= capacity) {
        return 0;
    }

    while (length != 0 &&
           path[length - 1U] != '\\' &&
           path[length - 1U] != '/') {
        --length;
    }

    file_name_length = (unsigned int)strlen(file_name) + 1U;
    if ((length + file_name_length) > capacity) {
        return 0;
    }

    CopyMemory(path + length, file_name, file_name_length);
    return 1;
}

static int helper_build_config_path(char *path, unsigned int capacity)
{
    return helper_build_module_file_path(path, capacity, "vmodem.ini");
}

static int helper_build_log_path(char *path, unsigned int capacity)
{
    return helper_build_module_file_path(path,
                                         capacity,
                                         HELPER_SESSION_LOG_FILE_NAME);
}

static void helper_show_error(HWND owner, const char *title, DWORD detail)
{
    char msg[256];

    wsprintfA(msg, "%s\r\n\r\nCode: %lu", title, (unsigned long)detail);
    MessageBoxA(owner, msg, HELPER_APP_NAME, MB_OK | MB_ICONERROR);
}

static void helper_show_message(HWND owner,
                                const char *title,
                                const char *message,
                                UINT flags)
{
    MessageBoxA(owner, message, title, flags);
}

static int helper_register_window_classes(HINSTANCE hInstance)
{
    WNDCLASSA wc;

    if (!g_helper_main_class_registered) {
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = helper_main_window_proc;
        wc.hInstance = hInstance;
        wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = HELPER_APP_MAIN_CLASS;
        if (!RegisterClassA(&wc)) {
            return 0;
        }
        g_helper_main_class_registered = 1;
    }

    if (!g_helper_entry_class_registered) {
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = helper_entry_window_proc;
        wc.hInstance = hInstance;
        wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = HELPER_APP_ENTRY_CLASS;
        if (!RegisterClassA(&wc)) {
            return 0;
        }
        g_helper_entry_class_registered = 1;
    }

    return 1;
}

static void helper_app_set_font(HWND hwnd)
{
    HFONT font;

    if (hwnd == NULL) {
        return;
    }

    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageA(hwnd, WM_SETFONT, (WPARAM)font, MAKELPARAM(TRUE, 0));
}

static void helper_app_sync_listener_controls(HELPER_APP *app)
{
    char port_text[16];

    if (app == NULL) {
        return;
    }

    app->suppress_listener_updates = 1;
    SendMessageA(app->hwnd_listener_enabled,
                 BM_SETCHECK,
                 app->config.listener.enabled ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageA(app->hwnd_logging_enabled,
                 BM_SETCHECK,
                 app->config.logging.enabled ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SetWindowTextA(app->hwnd_listener_host, app->config.listener.bind_host);
    wsprintfA(port_text, "%u", (unsigned int)app->config.listener.port);
    SetWindowTextA(app->hwnd_listener_port, port_text);
    app->suppress_listener_updates = 0;
}

static void helper_app_refresh_phonebook_list(HELPER_APP *app)
{
    LVCOLUMNA column;
    LVITEMA item;
    unsigned int i;
    HWND header;

    if (app == NULL || app->hwnd_phonebook_list == NULL) {
        return;
    }

    header = ListView_GetHeader(app->hwnd_phonebook_list);
    if (header == NULL || Header_GetItemCount(header) == 0) {
        ZeroMemory(&column, sizeof(column));
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        column.pszText = "Name";
        column.cx = 160;
        column.iSubItem = 0;
        ListView_InsertColumn(app->hwnd_phonebook_list, 0, &column);

        column.pszText = "Number";
        column.cx = 150;
        column.iSubItem = 1;
        ListView_InsertColumn(app->hwnd_phonebook_list, 1, &column);

        column.pszText = "Target";
        column.cx = 210;
        column.iSubItem = 2;
        ListView_InsertColumn(app->hwnd_phonebook_list, 2, &column);

        column.pszText = "Raw";
        column.cx = 60;
        column.iSubItem = 3;
        ListView_InsertColumn(app->hwnd_phonebook_list, 3, &column);
    }

    ListView_DeleteAllItems(app->hwnd_phonebook_list);

    for (i = 0; i < app->config.phonebook_count; ++i) {
        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = app->config.phonebook[i].name;
        ListView_InsertItem(app->hwnd_phonebook_list, &item);
        ListView_SetItemText(app->hwnd_phonebook_list,
                             (int)i,
                             1,
                             app->config.phonebook[i].number);
        ListView_SetItemText(app->hwnd_phonebook_list,
                             (int)i,
                             2,
                             app->config.phonebook[i].target);
        ListView_SetItemText(app->hwnd_phonebook_list,
                             (int)i,
                             3,
                             app->config.phonebook[i].raw_mode ? "On" : "Off");
    }
}

static void helper_app_update_status(HELPER_APP *app)
{
    char text[256];

    if (app == NULL) {
        return;
    }

    if (!app->config.listener.enabled) {
        lstrcpyA(text, "Listener disabled");
    } else if (app->net.listen_socket != INVALID_SOCKET) {
        wsprintfA(text,
                  "Listener active on %s:%u",
                  app->config.listener.bind_host,
                  (unsigned int)app->config.listener.port);
    } else {
        wsprintfA(text,
                  "Listener enabled for %s:%u but not currently listening",
                  app->config.listener.bind_host,
                  (unsigned int)app->config.listener.port);
    }
    SetWindowTextA(app->hwnd_listener_status, text);

    if (app->config.logging.enabled) {
        wsprintfA(text,
                  "Session logging enabled: %s",
                  helper_session_log_path(&app->session_log));
    } else {
        lstrcpyA(text, "Session logging disabled");
    }
    SetWindowTextA(app->hwnd_logging_status, text);

    if (app->pending_connect_valid) {
        char pending_number[HELPER_PHONEBOOK_NUMBER_LEN];

        if (!helper_phonebook_payload_is_phone_like(
                app->pending_connect_message.payload,
                app->pending_connect_message.payload_length,
                pending_number,
                sizeof(pending_number),
                NULL,
                0U)) {
            lstrcpyA(pending_number, "(unknown)");
        }
        wsprintfA(text,
                  "Waiting for phonebook entry for %s",
                  pending_number);
    } else if (app->net.pending_session_id != 0UL) {
        switch (app->net.pending_mode) {
        case HELPER_NET_PENDING_MODE_PROBE:
            wsprintfA(text,
                      "Inbound call negotiating, session %lu pending",
                      app->net.pending_session_id);
            break;

        case HELPER_NET_PENDING_MODE_DELAYED:
            wsprintfA(text,
                      "Inbound delayed-connect ringing, session %lu pending",
                      app->net.pending_session_id);
            break;

        default:
            wsprintfA(text,
                      "Inbound call ringing, session %lu pending",
                      app->net.pending_session_id);
            break;
        }
    } else {
        switch (app->net.state) {
        case HELPER_NET_STATE_RESOLVING:
            wsprintfA(text,
                      "Resolving %s:%u",
                      app->net.target.host,
                      (unsigned int)app->net.target.port);
            break;

        case HELPER_NET_STATE_CONNECTING:
            wsprintfA(text,
                      "Connecting to %s:%u",
                      app->net.target.host,
                      (unsigned int)app->net.target.port);
            break;

        case HELPER_NET_STATE_WAIT_HELLO_ACK:
            wsprintfA(text,
                      "Negotiating delayed connect with %s:%u",
                      app->net.target.host,
                      (unsigned int)app->net.target.port);
            break;

        case HELPER_NET_STATE_WAIT_ANSWERED:
            wsprintfA(text,
                      "Waiting for remote answer from %s:%u",
                      app->net.target.host,
                      (unsigned int)app->net.target.port);
            break;

        case HELPER_NET_STATE_CONNECTED:
            wsprintfA(text,
                      "Connected, session %lu active",
                      app->net.active_session_id);
            break;

        default:
            lstrcpyA(text, "Idle and ready");
            break;
        }
    }

    SetWindowTextA(app->hwnd_runtime_status, text);
}

static void helper_app_log_event(HELPER_APP *app,
                                 const char *event_name,
                                 unsigned long session_id,
                                 const char *detail)
{
    if (app == NULL) {
        return;
    }

    helper_session_log_event(&app->session_log, event_name, session_id, detail);
}

static void helper_app_log_protocol(HELPER_APP *app,
                                    const char *direction,
                                    const VMODEM_PROTOCOL_MESSAGE *message)
{
    if (app == NULL || message == NULL) {
        return;
    }

    helper_session_log_protocol(&app->session_log, direction, message);
}

static void helper_app_log_vxd_trace_line(HELPER_APP *app,
                                          const char *text)
{
    if (app == NULL) {
        return;
    }

    helper_session_log_text(&app->session_log, "VXD_TRACE", text);
}

static int helper_app_drain_vxd_trace(HELPER_APP *app)
{
    VMODEM_GET_TRACE_LOG_ACK ack;
    char line[VMODEM_TRACE_LOG_DATA_LEN + 1U];
    char detail[64];
    DWORD start;
    DWORD i;
    DWORD line_len;
    DWORD error_code;

    if (app == NULL ||
        !helper_session_log_is_enabled(&app->session_log) ||
        app->vxd_trace_poll_state < 0) {
        return 1;
    }

    ZeroMemory(&ack, sizeof(ack));
    if (!IPC_GetTraceLog(app->hDevice, VMODEM_IPC_VERSION, &ack)) {
        error_code = GetLastError();
        if (error_code == ERROR_NOT_SUPPORTED ||
            error_code == ERROR_INVALID_FUNCTION) {
            app->vxd_trace_poll_state = -1;
            if (app->last_trace_poll_error != error_code) {
                wsprintfA(detail, "trace spool unavailable code=%lu",
                          (unsigned long)error_code);
                helper_app_log_vxd_trace_line(app, detail);
                app->last_trace_poll_error = error_code;
            }
            return 1;
        }

        if (app->last_trace_poll_error != error_code) {
            wsprintfA(detail, "trace spool poll failed code=%lu",
                      (unsigned long)error_code);
            helper_app_log_vxd_trace_line(app, detail);
            app->last_trace_poll_error = error_code;
        }
        return 1;
    }

    app->vxd_trace_poll_state = 1;
    app->last_trace_poll_error = 0;

    if (ack.status != VMODEM_STATUS_OK) {
        wsprintfA(detail, "trace spool status=%lu", ack.status);
        helper_app_log_vxd_trace_line(app, detail);
        return 1;
    }

    if (ack.dropped_count != 0UL) {
        wsprintfA(detail, "trace lines dropped=%lu", ack.dropped_count);
        helper_app_log_vxd_trace_line(app, detail);
    }

    if (ack.log_len == 0U) {
        return 1;
    }

    if (ack.log_len > VMODEM_TRACE_LOG_DATA_LEN) {
        ack.log_len = VMODEM_TRACE_LOG_DATA_LEN;
    }

    start = 0;
    for (i = 0; i <= ack.log_len; ++i) {
        if (i == ack.log_len ||
            ack.log_data[i] == '\n' ||
            ack.log_data[i] == '\r') {
            line_len = i - start;
            if (line_len != 0U) {
                if (line_len > VMODEM_TRACE_LOG_DATA_LEN) {
                    line_len = VMODEM_TRACE_LOG_DATA_LEN;
                }
                CopyMemory(line, ack.log_data + start, line_len);
                line[line_len] = '\0';
                helper_app_log_vxd_trace_line(app, line);
            }

            while ((i + 1U) < ack.log_len &&
                   (ack.log_data[i + 1U] == '\n' ||
                    ack.log_data[i + 1U] == '\r')) {
                ++i;
            }
            start = i + 1U;
        }
    }

    return 1;
}

static void helper_app_remove_tray_icon(HELPER_APP *app)
{
    NOTIFYICONDATAA nid;

    if (app == NULL || !app->tray_added || app->hwnd_main == NULL) {
        return;
    }

    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = NOTIFYICONDATA_V1_SIZE;
    nid.hWnd = app->hwnd_main;
    nid.uID = HELPER_APP_TRAY_ID;
    Shell_NotifyIconA(NIM_DELETE, &nid);
    app->tray_added = 0;
}

static int helper_app_hide_to_tray(HELPER_APP *app)
{
    NOTIFYICONDATAA nid;

    if (app == NULL || app->hwnd_main == NULL) {
        return 0;
    }

    if (!app->tray_added) {
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = NOTIFYICONDATA_V1_SIZE;
        nid.hWnd = app->hwnd_main;
        nid.uID = HELPER_APP_TRAY_ID;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = HELPER_APP_WM_TRAY;
        nid.hIcon = LoadIconA(NULL, IDI_APPLICATION);
        lstrcpyA(nid.szTip, HELPER_APP_NAME);
        if (!Shell_NotifyIconA(NIM_ADD, &nid)) {
            helper_show_error(app->hwnd_main,
                              "Failed to add tray icon.",
                              GetLastError());
            return 0;
        }
        app->tray_added = 1;
    }

    ShowWindow(app->hwnd_main, SW_HIDE);
    return 1;
}

static void helper_app_restore_window(HELPER_APP *app)
{
    if (app == NULL || app->hwnd_main == NULL) {
        return;
    }

    helper_app_remove_tray_icon(app);
    ShowWindow(app->hwnd_main, SW_SHOW);
    ShowWindow(app->hwnd_main, SW_RESTORE);
    SetForegroundWindow(app->hwnd_main);
}

static void helper_app_clear_pending_connect(HELPER_APP *app, int close_dialog)
{
    if (app == NULL) {
        return;
    }

    app->pending_connect_valid = 0;
    ZeroMemory(&app->pending_connect_message, sizeof(app->pending_connect_message));

    if (close_dialog &&
        app->active_dialog != NULL &&
        app->active_dialog->hwnd != NULL) {
        PostMessageA(app->active_dialog->hwnd, WM_CLOSE, 0, 0);
    }
}

static int helper_app_retry_pending_dial(HELPER_APP *app)
{
    VMODEM_PROTOCOL_MESSAGE message;
    char target_text[HELPER_PHONEBOOK_TARGET_LEN];
    char normalized[HELPER_PHONEBOOK_NORMALIZED_LEN];
    char detail[HELPER_PHONEBOOK_TARGET_LEN + HELPER_PHONEBOOK_NUMBER_LEN + 32];
    unsigned int phonebook_index;
    int resolution;

    if (app == NULL || !app->pending_connect_valid) {
        return 1;
    }

    phonebook_index = HELPER_PHONEBOOK_MAX_ENTRIES;
    resolution = helper_phonebook_resolve_dial(&app->config,
                                               app->pending_connect_message.payload,
                                               app->pending_connect_message.payload_length,
                                               target_text,
                                               sizeof(target_text),
                                               normalized,
                                               sizeof(normalized),
                                               &phonebook_index);
    if (resolution != HELPER_DIAL_RESOLUTION_DIRECT &&
        resolution != HELPER_DIAL_RESOLUTION_PHONEBOOK) {
        return 0;
    }

    message = app->pending_connect_message;
    message.payload_length = (unsigned long)strlen(target_text);
    ZeroMemory(message.payload, sizeof(message.payload));
    CopyMemory(message.payload, target_text, message.payload_length);
    if (resolution == HELPER_DIAL_RESOLUTION_PHONEBOOK &&
        phonebook_index < app->config.phonebook_count) {
        if (app->config.phonebook[phonebook_index].raw_mode) {
            message.status |= VMODEM_CONNECT_FLAG_RAW;
        } else {
            message.status &= ~VMODEM_CONNECT_FLAG_RAW;
        }
    }
    wsprintfA(detail,
              "number=%s target=%s raw=%lu",
              normalized,
              target_text,
              (message.status & VMODEM_CONNECT_FLAG_RAW) ? 1UL : 0UL);
    helper_app_log_event(app,
                         "PHONEBOOK_RETRY",
                         message.session_id,
                         detail);
    helper_app_clear_pending_connect(app, 0);
    return helper_app_process_message(app, &message, 0, NULL);
}

static int helper_app_commit_phonebook_entry(HELPER_APP *app,
                                             int entry_index,
                                             const HELPER_PHONEBOOK_ENTRY *entry)
{
    HELPER_APP_CONFIG updated;
    int selected_index;
    char detail[HELPER_PHONEBOOK_NAME_LEN + HELPER_PHONEBOOK_NUMBER_LEN +
                HELPER_PHONEBOOK_TARGET_LEN + 48];

    if (app == NULL || entry == NULL) {
        return 0;
    }

    updated = app->config;

    if (entry_index == HELPER_ENTRY_EDIT_ADD) {
        if (updated.phonebook_count >= HELPER_PHONEBOOK_MAX_ENTRIES) {
            helper_show_message(app->hwnd_main,
                                HELPER_APP_NAME,
                                "The phonebook is full.",
                                MB_OK | MB_ICONWARNING);
            return 0;
        }
        updated.phonebook[updated.phonebook_count] = *entry;
        ++updated.phonebook_count;
        selected_index = (int)(updated.phonebook_count - 1U);
    } else {
        if (entry_index < 0 ||
            (unsigned int)entry_index >= updated.phonebook_count) {
            return 0;
        }
        updated.phonebook[entry_index] = *entry;
        selected_index = entry_index;
    }

    if (!helper_app_config_validate(&updated)) {
        helper_show_message(app->hwnd_main,
                            HELPER_APP_NAME,
                            "The phonebook entry is invalid, duplicates an existing number, or has an invalid target.",
                            MB_OK | MB_ICONWARNING);
        return 0;
    }

    if (!helper_app_config_save_file(app->config_path, &updated)) {
        helper_show_error(app->hwnd_main,
                          "Failed to save vmodem.ini.",
                          GetLastError());
        return 0;
    }

    app->config = updated;
    helper_app_refresh_phonebook_list(app);
    helper_app_select_phonebook_index(app, selected_index);
    helper_app_update_status(app);
    wsprintfA(detail,
              "name=%s number=%s target=%s raw=%d",
              entry->name,
              entry->number,
              entry->target,
              entry->raw_mode ? 1 : 0);
    helper_app_log_event(app,
                         (entry_index == HELPER_ENTRY_EDIT_ADD) ?
                             "PHONEBOOK_ADD" :
                             "PHONEBOOK_EDIT",
                         0UL,
                         detail);
    return 1;
}

static int helper_entry_dialog_run(HELPER_APP *app,
                                   const char *title,
                                   const HELPER_PHONEBOOK_ENTRY *initial_entry,
                                   int lock_number,
                                   HELPER_PHONEBOOK_ENTRY *result)
{
    HELPER_ENTRY_DIALOG_STATE dialog_state;
    MSG msg;

    if (app == NULL || title == NULL || initial_entry == NULL || result == NULL) {
        return 0;
    }

    ZeroMemory(&dialog_state, sizeof(dialog_state));
    dialog_state.app = app;
    lstrcpynA(dialog_state.title, title, sizeof(dialog_state.title));
    dialog_state.result = *initial_entry;
    dialog_state.number_locked = lock_number;

    app->active_dialog = &dialog_state;
    EnableWindow(app->hwnd_main, FALSE);

    dialog_state.hwnd = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                        HELPER_APP_ENTRY_CLASS,
                                        dialog_state.title,
                                        WS_CAPTION | WS_POPUP | WS_SYSMENU,
                                        CW_USEDEFAULT,
                                        CW_USEDEFAULT,
                                        380,
                                        220,
                                        app->hwnd_main,
                                        NULL,
                                        app->hInstance,
                                        &dialog_state);
    if (dialog_state.hwnd == NULL) {
        EnableWindow(app->hwnd_main, TRUE);
        app->active_dialog = NULL;
        return 0;
    }

    ShowWindow(dialog_state.hwnd, SW_SHOW);
    UpdateWindow(dialog_state.hwnd);

    while (!dialog_state.done) {
        if (GetMessageA(&msg, NULL, 0, 0) <= 0) {
            dialog_state.done = 1;
            dialog_state.accepted = 0;
            break;
        }

        if (dialog_state.hwnd != NULL && IsDialogMessageA(dialog_state.hwnd, &msg)) {
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    EnableWindow(app->hwnd_main, TRUE);
    SetForegroundWindow(app->hwnd_main);
    app->active_dialog = NULL;

    if (dialog_state.accepted) {
        *result = dialog_state.result;
    }

    return dialog_state.accepted;
}

static int helper_app_edit_phonebook_entry(HELPER_APP *app,
                                           int entry_index,
                                           const HELPER_PHONEBOOK_ENTRY *initial_entry,
                                           int lock_number,
                                           const char *title,
                                           int retry_pending_after_save)
{
    HELPER_PHONEBOOK_ENTRY candidate;

    if (app == NULL || initial_entry == NULL || title == NULL) {
        return 0;
    }

    candidate = *initial_entry;
    for (;;) {
        if (!helper_entry_dialog_run(app,
                                     title,
                                     &candidate,
                                     lock_number,
                                     &candidate)) {
            return 0;
        }

        if (!helper_app_commit_phonebook_entry(app, entry_index, &candidate)) {
            continue;
        }

        if (retry_pending_after_save &&
            app->pending_connect_valid &&
            !helper_app_retry_pending_dial(app)) {
            return 0;
        }

        return 1;
    }
}

static int helper_app_prompt_unknown_number(HELPER_APP *app)
{
    HELPER_PHONEBOOK_ENTRY initial_entry;
    char dialed_number[HELPER_PHONEBOOK_NUMBER_LEN];

    if (app == NULL || !app->pending_connect_valid) {
        return 1;
    }

    helper_phonebook_entry_init(&initial_entry);
    if (!helper_phonebook_payload_is_phone_like(
            app->pending_connect_message.payload,
            app->pending_connect_message.payload_length,
            dialed_number,
            sizeof(dialed_number),
            NULL,
            0U)) {
        lstrcpyA(dialed_number, "");
    }
    lstrcpynA(initial_entry.number, dialed_number, sizeof(initial_entry.number));

    helper_app_restore_window(app);
    helper_app_log_event(app,
                         "PHONEBOOK_PROMPT",
                         app->pending_connect_message.session_id,
                         dialed_number);
    if (!helper_app_edit_phonebook_entry(app,
                                         HELPER_ENTRY_EDIT_ADD,
                                         &initial_entry,
                                         1,
                                         "Add Phonebook Entry",
                                         1)) {
        if (app->pending_connect_valid) {
            helper_app_log_event(app,
                                 "PHONEBOOK_PROMPT_CANCEL",
                                 app->pending_connect_message.session_id,
                                 dialed_number);
            if (!helper_net_runtime_submit_connect_fail(
                    &app->net,
                    app->pending_connect_message.session_id,
                    VMODEM_CONNECT_FAIL_LOCAL)) {
                return 0;
            }
            helper_app_clear_pending_connect(app, 0);
        }
        return 1;
    }

    return 1;
}

static int helper_app_check_runtime_error(HELPER_APP *app)
{
    if (app == NULL) {
        return 0;
    }

    if (!helper_net_runtime_has_fatal_error(&app->net)) {
        return 1;
    }

    SetLastError(helper_net_runtime_get_fatal_error(&app->net));
    return 0;
}

static int helper_app_process_net_protocol(HELPER_APP *app,
                                           const VMODEM_PROTOCOL_MESSAGE *message,
                                           int isDeferred,
                                           int *pStillDeferred)
{
    int result;

    if (pStillDeferred != NULL) {
        *pStillDeferred = 0;
    }

    if (app == NULL || message == NULL) {
        return 0;
    }

    result = helper_net_runtime_handle_protocol(&app->net, message);
    if (result == HELPER_NET_PROTOCOL_FATAL) {
        return 0;
    }

    if (result == HELPER_NET_PROTOCOL_DEFER) {
        if (isDeferred) {
            if (pStillDeferred != NULL) {
                *pStillDeferred = 1;
            }
            return 1;
        }

        if (!app->deferred_valid) {
            app->deferred_message = *message;
            app->deferred_valid = 1;
        } else if (message->message_type == VMODEM_MSG_DATA_TO_NET) {
            OutputDebugStringA(
                "VMODEM helper: dropping newest outbound chunk while send buffer is saturated\r\n");
        }
    }

    return 1;
}

static int helper_app_process_message(HELPER_APP *app,
                                      const VMODEM_PROTOCOL_MESSAGE *message,
                                      int isDeferred,
                                      int *pStillDeferred)
{
    VMODEM_PROTOCOL_MESSAGE translated;
    char target_text[HELPER_PHONEBOOK_TARGET_LEN];
    char normalized[HELPER_PHONEBOOK_NORMALIZED_LEN];
    char detail[HELPER_PHONEBOOK_TARGET_LEN + HELPER_PHONEBOOK_NUMBER_LEN + 48];
    unsigned int phonebook_index;
    int resolution;

    if (pStillDeferred != NULL) {
        *pStillDeferred = 0;
    }

    if (app == NULL || message == NULL) {
        return 0;
    }

    if (!isDeferred) {
        helper_app_log_protocol(app, "IPC_RX", message);
    }

    if (app->pending_connect_valid &&
        message->message_type == VMODEM_MSG_HANGUP_REQ &&
        message->session_id == app->pending_connect_message.session_id) {
        helper_app_clear_pending_connect(app, 1);
        return 1;
    }

    if (message->message_type != VMODEM_MSG_CONNECT_REQ) {
        if (message->message_type == VMODEM_MSG_DATA_TO_NET &&
            message->payload_length != 0U) {
            helper_session_log_bytes(&app->session_log,
                                     "SERIAL_TX",
                                     message->session_id,
                                     message->payload,
                                     (unsigned short)message->payload_length);
        }
        return helper_app_process_net_protocol(app, message, isDeferred, pStillDeferred);
    }

    if (app->pending_connect_valid) {
        return helper_net_runtime_submit_connect_fail(&app->net,
                                                      message->session_id,
                                                      VMODEM_CONNECT_FAIL_LOCAL);
    }

    phonebook_index = HELPER_PHONEBOOK_MAX_ENTRIES;
    resolution = helper_phonebook_resolve_dial(&app->config,
                                               message->payload,
                                               message->payload_length,
                                               target_text,
                                               sizeof(target_text),
                                               normalized,
                                               sizeof(normalized),
                                               &phonebook_index);
    if (resolution == HELPER_DIAL_RESOLUTION_DIRECT ||
        resolution == HELPER_DIAL_RESOLUTION_PHONEBOOK) {
        translated = *message;
        if (resolution == HELPER_DIAL_RESOLUTION_PHONEBOOK &&
            phonebook_index < app->config.phonebook_count) {
            if (app->config.phonebook[phonebook_index].raw_mode) {
                translated.status |= VMODEM_CONNECT_FLAG_RAW;
            } else {
                translated.status &= ~VMODEM_CONNECT_FLAG_RAW;
            }
        }
        if (resolution == HELPER_DIAL_RESOLUTION_DIRECT) {
            wsprintfA(detail,
                      "target=%s raw=%lu",
                      target_text,
                      (translated.status & VMODEM_CONNECT_FLAG_RAW) ? 1UL : 0UL);
            helper_app_log_event(app,
                                 "DIAL_DIRECT",
                                 message->session_id,
                                 detail);
        } else {
            wsprintfA(detail,
                      "number=%s target=%s raw=%lu",
                      normalized,
                      target_text,
                      (translated.status & VMODEM_CONNECT_FLAG_RAW) ? 1UL : 0UL);
            helper_app_log_event(app,
                                 "PHONEBOOK_MATCH",
                                 message->session_id,
                                 detail);
        }
        translated.payload_length = (unsigned long)strlen(target_text);
        ZeroMemory(translated.payload, sizeof(translated.payload));
        CopyMemory(translated.payload, target_text, translated.payload_length);
        return helper_app_process_net_protocol(app,
                                               &translated,
                                               isDeferred,
                                               pStillDeferred);
    }

    if (resolution == HELPER_DIAL_RESOLUTION_PROMPT) {
        helper_app_log_event(app,
                             "PHONEBOOK_MISS",
                             message->session_id,
                             normalized);
        app->pending_connect_message = *message;
        app->pending_connect_valid = 1;
        return helper_app_prompt_unknown_number(app);
    }

    helper_app_log_event(app,
                         "DIAL_INVALID",
                         message->session_id,
                         NULL);
    return helper_net_runtime_submit_connect_fail(&app->net,
                                                  message->session_id,
                                                  VMODEM_CONNECT_FAIL_LOCAL);
}

static int helper_app_retry_deferred(HELPER_APP *app)
{
    int ok;
    int stillDeferred;

    if (app == NULL || !app->deferred_valid) {
        return 1;
    }

    stillDeferred = 0;
    ok = helper_app_process_message(app,
                                    &app->deferred_message,
                                    1,
                                    &stillDeferred);
    if (!ok) {
        return 0;
    }

    if (!stillDeferred) {
        app->deferred_valid = 0;
        ZeroMemory(&app->deferred_message, sizeof(app->deferred_message));
    }

    return 1;
}

static int helper_app_drain_ipc(HELPER_APP *app)
{
    VMODEM_PROTOCOL_MESSAGE message;

    if (app == NULL) {
        return 0;
    }

    for (;;) {
        ZeroMemory(&message, sizeof(message));
        if (!IPC_ReceiveMessage(app->hDevice,
                                VMODEM_IPC_VERSION,
                                app->helper_generation,
                                &message)) {
            return 0;
        }

        /* RECEIVE_MESSAGE reuses the protocol message struct. Only a
         * VMODEM_MSG_NONE reply means message.status is an IPC receive status;
         * real protocol messages are allowed to use status for payload data
         * such as CONNECT_REQ flags.
         */
        if (message.message_type == VMODEM_MSG_NONE) {
            if (message.status == VMODEM_STATUS_NO_MESSAGE) {
                return 1;
            }

            if (message.status == VMODEM_STATUS_STALE ||
                message.status == VMODEM_STATUS_NOT_OWNER) {
                SetLastError((DWORD)message.status);
                return 0;
            }

            if (message.status != VMODEM_STATUS_OK) {
                SetLastError((DWORD)message.status);
                return 0;
            }
        }

        if (!helper_app_process_message(app, &message, 0, NULL)) {
            return 0;
        }
    }
}

static int helper_app_get_selected_phonebook_index(HELPER_APP *app)
{
    if (app == NULL || app->hwnd_phonebook_list == NULL) {
        return -1;
    }

    return ListView_GetNextItem(app->hwnd_phonebook_list, -1, LVNI_SELECTED);
}

static void helper_app_select_phonebook_index(HELPER_APP *app, int index)
{
    if (app == NULL ||
        app->hwnd_phonebook_list == NULL ||
        index < 0) {
        return;
    }

    ListView_SetItemState(app->hwnd_phonebook_list,
                          index,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(app->hwnd_phonebook_list, index, FALSE);
}

static int helper_app_apply_listener_from_controls(HELPER_APP *app)
{
    HELPER_NET_LISTENER_CONFIG candidate;
    HELPER_LOG_CONFIG logging_candidate;
    HELPER_APP_CONFIG updated;
    HELPER_NET_LISTENER_CONFIG previous_listener;
    char host[HELPER_NET_HOST_BUFFER_LEN];
    char port_text[16];
    unsigned long port_value;
    unsigned long i;
    unsigned char ch;
    int previous_logging_enabled;

    if (app == NULL || app->suppress_listener_updates) {
        return 1;
    }

    helper_net_listener_config_init(&candidate);
    candidate.enabled =
        (SendMessageA(app->hwnd_listener_enabled, BM_GETCHECK, 0, 0) ==
         BST_CHECKED) ? 1 : 0;

    GetWindowTextA(app->hwnd_listener_host, host, sizeof(host));
    if (host[0] == '\0') {
        helper_show_message(app->hwnd_main,
                            HELPER_APP_NAME,
                            "Bind host cannot be empty.",
                            MB_OK | MB_ICONWARNING);
        helper_app_sync_listener_controls(app);
        return 0;
    }

    for (i = 0; host[i] != '\0'; ++i) {
        ch = (unsigned char)host[i];
        if (ch < 0x21U || ch > 0x7EU) {
            helper_show_message(app->hwnd_main,
                                HELPER_APP_NAME,
                                "Bind host contains invalid characters.",
                                MB_OK | MB_ICONWARNING);
            helper_app_sync_listener_controls(app);
            return 0;
        }
    }
    lstrcpynA(candidate.bind_host, host, sizeof(candidate.bind_host));

    GetWindowTextA(app->hwnd_listener_port, port_text, sizeof(port_text));
    if (port_text[0] == '\0') {
        helper_show_message(app->hwnd_main,
                            HELPER_APP_NAME,
                            "Port cannot be empty.",
                            MB_OK | MB_ICONWARNING);
        helper_app_sync_listener_controls(app);
        return 0;
    }

    port_value = 0UL;
    for (i = 0; port_text[i] != '\0'; ++i) {
        ch = (unsigned char)port_text[i];
        if (ch < '0' || ch > '9') {
            helper_show_message(app->hwnd_main,
                                HELPER_APP_NAME,
                                "Port must be numeric.",
                                MB_OK | MB_ICONWARNING);
            helper_app_sync_listener_controls(app);
            return 0;
        }
        port_value = (port_value * 10UL) + (unsigned long)(ch - '0');
        if (port_value > 65535UL) {
            helper_show_message(app->hwnd_main,
                                HELPER_APP_NAME,
                                "Port must be between 1 and 65535.",
                                MB_OK | MB_ICONWARNING);
            helper_app_sync_listener_controls(app);
            return 0;
        }
    }

    if (port_value == 0UL) {
        helper_show_message(app->hwnd_main,
                            HELPER_APP_NAME,
                            "Port must be between 1 and 65535.",
                            MB_OK | MB_ICONWARNING);
        helper_app_sync_listener_controls(app);
        return 0;
    }

    candidate.port = (unsigned short)port_value;
    helper_log_config_init(&logging_candidate);
    logging_candidate.enabled =
        (SendMessageA(app->hwnd_logging_enabled, BM_GETCHECK, 0, 0) ==
         BST_CHECKED) ? 1 : 0;

    if (candidate.enabled == app->config.listener.enabled &&
        lstrcmpA(candidate.bind_host, app->config.listener.bind_host) == 0 &&
        candidate.port == app->config.listener.port &&
        logging_candidate.enabled == app->config.logging.enabled) {
        return 1;
    }

    updated = app->config;
    updated.listener = candidate;
    updated.logging = logging_candidate;
    if (!helper_app_config_validate(&updated)) {
        helper_show_message(app->hwnd_main,
                            HELPER_APP_NAME,
                            "The listener or logging settings are invalid.",
                            MB_OK | MB_ICONWARNING);
        helper_app_sync_listener_controls(app);
        return 0;
    }

    previous_listener = app->config.listener;
    previous_logging_enabled = app->config.logging.enabled;

    if (!helper_session_log_apply(&app->session_log,
                                  app->log_path,
                                  logging_candidate.enabled)) {
        helper_show_error(app->hwnd_main,
                          "Failed to open the session log file.",
                          GetLastError());
        helper_app_sync_listener_controls(app);
        return 0;
    }

    if (!helper_net_runtime_apply_listener_config(&app->net, &candidate)) {
        helper_show_error(app->hwnd_main,
                          "Failed to apply listener settings.",
                          GetLastError());
        helper_session_log_apply(&app->session_log,
                                 app->log_path,
                                 previous_logging_enabled);
        helper_app_sync_listener_controls(app);
        return 0;
    }

    if (!helper_app_config_save_file(app->config_path, &updated)) {
        helper_net_runtime_apply_listener_config(&app->net, &previous_listener);
        helper_session_log_apply(&app->session_log,
                                 app->log_path,
                                 previous_logging_enabled);
        helper_show_error(app->hwnd_main,
                          "Failed to save vmodem.ini.",
                          GetLastError());
        helper_app_sync_listener_controls(app);
        return 0;
    }

    app->config = updated;
    helper_app_update_status(app);
    helper_app_log_event(app,
                         app->config.logging.enabled ?
                             "SESSION_LOG_ENABLED" :
                             "SESSION_LOG_DISABLED",
                         0UL,
                         helper_session_log_path(&app->session_log));
    return 1;
}

static void helper_app_request_exit(HELPER_APP *app)
{
    if (app == NULL || app->hwnd_main == NULL) {
        return;
    }

    app->exiting = 1;
    DestroyWindow(app->hwnd_main);
}

static int helper_app_initialize(HELPER_APP *app, HINSTANCE hInstance)
{
    VMODEM_HELLO_ACK helloAck;
    INITCOMMONCONTROLSEX icc;
    HWND hwnd;
    int result;

    if (app == NULL) {
        return 0;
    }

    ZeroMemory(app, sizeof(*app));
    app->hInstance = hInstance;
    app->hDevice = INVALID_HANDLE_VALUE;
    helper_session_log_init(&app->session_log);

    if (!helper_build_config_path(app->config_path, sizeof(app->config_path))) {
        SetLastError(ERROR_GEN_FAILURE);
        return 0;
    }
    if (!helper_build_log_path(app->log_path, sizeof(app->log_path))) {
        SetLastError(ERROR_GEN_FAILURE);
        return 0;
    }

    helper_app_config_init(&app->config);
    if (!helper_app_config_load_file(app->config_path, &app->config)) {
        OutputDebugStringA("VMODEM helper: using default configuration\r\n");
    }

    if (!helper_register_window_classes(hInstance)) {
        SetLastError(ERROR_GEN_FAILURE);
        return 0;
    }

    ZeroMemory(&icc, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    result = IPC_Open(&app->hDevice);
    if (result != 0) {
        SetLastError((DWORD)result);
        return 0;
    }

    ZeroMemory(&helloAck, sizeof(helloAck));
    result = IPC_ClaimHelper(app->hDevice, VMODEM_IPC_VERSION, &helloAck);
    if (!result) {
        return 0;
    }
    if (helloAck.status != VMODEM_STATUS_OK) {
        SetLastError((DWORD)helloAck.status);
        return 0;
    }

    app->helper_generation = helloAck.helper_generation;

    if (!helper_session_log_apply(&app->session_log,
                                  app->log_path,
                                  app->config.logging.enabled)) {
        return 0;
    }

    if (!helper_net_runtime_init(&app->net,
                                 hInstance,
                                 app->hDevice,
                                 app->helper_generation,
                                 &app->config.listener)) {
        return 0;
    }
    helper_net_runtime_set_session_log(&app->net, &app->session_log);

    g_helper_app = app;
    hwnd = CreateWindowA(HELPER_APP_MAIN_CLASS,
                         HELPER_APP_NAME,
                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                         CW_USEDEFAULT,
                         CW_USEDEFAULT,
                         670,
                         480,
                         NULL,
                         NULL,
                         hInstance,
                         app);
    if (hwnd == NULL) {
        SetLastError(ERROR_GEN_FAILURE);
        return 0;
    }

    app->hwnd_main = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetTimer(hwnd, HELPER_APP_TIMER_ID, HELPER_APP_TIMER_MS, NULL);
    helper_app_update_status(app);
    return 1;
}

static void helper_app_shutdown(HELPER_APP *app)
{
    if (app == NULL) {
        return;
    }

    helper_app_remove_tray_icon(app);
    if (app->hwnd_main != NULL) {
        KillTimer(app->hwnd_main, HELPER_APP_TIMER_ID);
        app->hwnd_main = NULL;
    }
    helper_net_runtime_shutdown(&app->net);
    helper_session_log_shutdown(&app->session_log);
    if (app->hDevice != INVALID_HANDLE_VALUE) {
        IPC_Close(app->hDevice);
        app->hDevice = INVALID_HANDLE_VALUE;
    }
    g_helper_app = NULL;
}

static void helper_app_create_main_controls(HELPER_APP *app)
{
    HWND hwnd;

    if (app == NULL || app->hwnd_main == NULL) {
        return;
    }

    hwnd = CreateWindowA("BUTTON",
                         "Listener",
                         WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                         10,
                         8,
                         635,
                         108,
                         app->hwnd_main,
                         (HMENU)0,
                         app->hInstance,
                         NULL);
    helper_app_set_font(hwnd);

    app->hwnd_listener_enabled = CreateWindowA("BUTTON",
                                               "Enabled",
                                               WS_CHILD | WS_VISIBLE |
                                                   WS_TABSTOP | BS_AUTOCHECKBOX,
                                               24,
                                               28,
                                               90,
                                               20,
                                               app->hwnd_main,
                                               (HMENU)HELPER_CTRL_LISTENER_ENABLED,
                                               app->hInstance,
                                               NULL);
    helper_app_set_font(app->hwnd_listener_enabled);

    app->hwnd_logging_enabled = CreateWindowA("BUTTON",
                                              "Session Log",
                                              WS_CHILD | WS_VISIBLE |
                                                  WS_TABSTOP | BS_AUTOCHECKBOX,
                                              24,
                                              54,
                                              100,
                                              20,
                                              app->hwnd_main,
                                              (HMENU)HELPER_CTRL_LOGGING_ENABLED,
                                              app->hInstance,
                                              NULL);
    helper_app_set_font(app->hwnd_logging_enabled);

    hwnd = CreateWindowA("STATIC",
                         "Bind Host:",
                         WS_CHILD | WS_VISIBLE,
                         130,
                         30,
                         60,
                         18,
                         app->hwnd_main,
                         (HMENU)0,
                         app->hInstance,
                         NULL);
    helper_app_set_font(hwnd);

    app->hwnd_listener_host = CreateWindowA("EDIT",
                                            "",
                                            WS_CHILD | WS_VISIBLE | WS_BORDER |
                                                WS_TABSTOP | ES_AUTOHSCROLL,
                                            194,
                                            26,
                                            230,
                                            22,
                                            app->hwnd_main,
                                            (HMENU)HELPER_CTRL_LISTENER_HOST,
                                            app->hInstance,
                                            NULL);
    helper_app_set_font(app->hwnd_listener_host);
    SendMessageA(app->hwnd_listener_host,
                 EM_SETLIMITTEXT,
                 HELPER_NET_HOST_BUFFER_LEN - 1U,
                 0);

    hwnd = CreateWindowA("STATIC",
                         "Port:",
                         WS_CHILD | WS_VISIBLE,
                         440,
                         30,
                         30,
                         18,
                         app->hwnd_main,
                         (HMENU)0,
                         app->hInstance,
                         NULL);
    helper_app_set_font(hwnd);

    app->hwnd_listener_port = CreateWindowA("EDIT",
                                            "",
                                            WS_CHILD | WS_VISIBLE | WS_BORDER |
                                                WS_TABSTOP | ES_AUTOHSCROLL,
                                            474,
                                            26,
                                            60,
                                            22,
                                            app->hwnd_main,
                                            (HMENU)HELPER_CTRL_LISTENER_PORT,
                                            app->hInstance,
                                            NULL);
    helper_app_set_font(app->hwnd_listener_port);
    SendMessageA(app->hwnd_listener_port, EM_SETLIMITTEXT, 5, 0);

    hwnd = CreateWindowA("BUTTON",
                         "Apply",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                         544,
                         25,
                         64,
                         24,
                         app->hwnd_main,
                         (HMENU)HELPER_CTRL_LISTENER_APPLY,
                         app->hInstance,
                         NULL);
    helper_app_set_font(hwnd);

    app->hwnd_listener_status = CreateWindowA("STATIC",
                                              "",
                                              WS_CHILD | WS_VISIBLE,
                                              130,
                                              56,
                                              494,
                                              18,
                                              app->hwnd_main,
                                              (HMENU)HELPER_CTRL_STATUS_LISTENER,
                                              app->hInstance,
                                              NULL);
    helper_app_set_font(app->hwnd_listener_status);

    app->hwnd_logging_status = CreateWindowA("STATIC",
                                             "",
                                             WS_CHILD | WS_VISIBLE,
                                             130,
                                             76,
                                             494,
                                             18,
                                             app->hwnd_main,
                                             (HMENU)HELPER_CTRL_STATUS_LOGGING,
                                             app->hInstance,
                                             NULL);
    helper_app_set_font(app->hwnd_logging_status);

    hwnd = CreateWindowA("BUTTON",
                         "Phonebook",
                         WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                         10,
                         122,
                         635,
                         286,
                         app->hwnd_main,
                         (HMENU)0,
                         app->hInstance,
                         NULL);
    helper_app_set_font(hwnd);

    app->hwnd_phonebook_list = CreateWindowExA(WS_EX_CLIENTEDGE,
                                               WC_LISTVIEWA,
                                               "",
                                               WS_CHILD | WS_VISIBLE |
                                                   WS_TABSTOP | LVS_REPORT |
                                                   LVS_SINGLESEL,
                                               24,
                                               144,
                                               600,
                                               210,
                                               app->hwnd_main,
                                               (HMENU)HELPER_CTRL_PHONEBOOK_LIST,
                                               app->hInstance,
                                               NULL);
    helper_app_set_font(app->hwnd_phonebook_list);
    ListView_SetExtendedListViewStyle(app->hwnd_phonebook_list,
                                      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    hwnd = CreateWindowA("BUTTON",
                         "Add",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                         24,
                         366,
                         72,
                         24,
                         app->hwnd_main,
                         (HMENU)HELPER_CTRL_PHONEBOOK_ADD,
                         app->hInstance,
                         NULL);
    helper_app_set_font(hwnd);

    hwnd = CreateWindowA("BUTTON",
                         "Edit",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                         104,
                         366,
                         72,
                         24,
                         app->hwnd_main,
                         (HMENU)HELPER_CTRL_PHONEBOOK_EDIT,
                         app->hInstance,
                         NULL);
    helper_app_set_font(hwnd);

    hwnd = CreateWindowA("BUTTON",
                         "Delete",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                         184,
                         366,
                         72,
                         24,
                         app->hwnd_main,
                         (HMENU)HELPER_CTRL_PHONEBOOK_DELETE,
                         app->hInstance,
                         NULL);
    helper_app_set_font(hwnd);

    app->hwnd_runtime_status = CreateWindowA("STATIC",
                                             "",
                                             WS_CHILD | WS_VISIBLE,
                                             24,
                                             418,
                                             600,
                                             18,
                                             app->hwnd_main,
                                             (HMENU)HELPER_CTRL_STATUS_RUNTIME,
                                             app->hInstance,
                                             NULL);
    helper_app_set_font(app->hwnd_runtime_status);

    helper_app_sync_listener_controls(app);
    helper_app_refresh_phonebook_list(app);
}

static void helper_app_handle_phonebook_add(HELPER_APP *app)
{
    HELPER_PHONEBOOK_ENTRY entry;

    if (app == NULL) {
        return;
    }

    helper_phonebook_entry_init(&entry);
    helper_app_edit_phonebook_entry(app,
                                    HELPER_ENTRY_EDIT_ADD,
                                    &entry,
                                    0,
                                    "Add Phonebook Entry",
                                    0);
}

static void helper_app_handle_phonebook_edit(HELPER_APP *app)
{
    int index;

    if (app == NULL) {
        return;
    }

    index = helper_app_get_selected_phonebook_index(app);
    if (index < 0 || (unsigned int)index >= app->config.phonebook_count) {
        helper_show_message(app->hwnd_main,
                            HELPER_APP_NAME,
                            "Select a phonebook entry first.",
                            MB_OK | MB_ICONINFORMATION);
        return;
    }

    helper_app_edit_phonebook_entry(app,
                                    index,
                                    &app->config.phonebook[index],
                                    0,
                                    "Edit Phonebook Entry",
                                    0);
}

static void helper_app_handle_phonebook_delete(HELPER_APP *app)
{
    HELPER_APP_CONFIG updated;
    int index;
    unsigned int i;

    if (app == NULL) {
        return;
    }

    index = helper_app_get_selected_phonebook_index(app);
    if (index < 0 || (unsigned int)index >= app->config.phonebook_count) {
        helper_show_message(app->hwnd_main,
                            HELPER_APP_NAME,
                            "Select a phonebook entry first.",
                            MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (MessageBoxA(app->hwnd_main,
                    "Delete the selected phonebook entry?",
                    HELPER_APP_NAME,
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }

    updated = app->config;
    for (i = (unsigned int)index; i + 1U < updated.phonebook_count; ++i) {
        updated.phonebook[i] = updated.phonebook[i + 1U];
    }
    ZeroMemory(&updated.phonebook[updated.phonebook_count - 1U],
               sizeof(updated.phonebook[0]));
    --updated.phonebook_count;

    if (!helper_app_config_save_file(app->config_path, &updated)) {
        helper_show_error(app->hwnd_main,
                          "Failed to save vmodem.ini.",
                          GetLastError());
        return;
    }

    app->config = updated;
    helper_app_refresh_phonebook_list(app);
    if ((unsigned int)index >= app->config.phonebook_count &&
        app->config.phonebook_count != 0U) {
        index = (int)(app->config.phonebook_count - 1U);
    }
    helper_app_select_phonebook_index(app, index);
    helper_app_update_status(app);
    helper_app_log_event(app, "PHONEBOOK_DELETE", 0UL, NULL);
}

static LRESULT CALLBACK helper_entry_window_proc(HWND hwnd,
                                                 UINT message,
                                                 WPARAM wParam,
                                                 LPARAM lParam)
{
    HELPER_ENTRY_DIALOG_STATE *dialog;
    HWND control;

    dialog = (g_helper_app != NULL) ? g_helper_app->active_dialog : NULL;

    switch (message) {
    case WM_CREATE:
    {
        CREATESTRUCTA *create_struct;
        HWND hwnd_label;

        create_struct = (CREATESTRUCTA *)lParam;
        dialog = (HELPER_ENTRY_DIALOG_STATE *)create_struct->lpCreateParams;
        if (dialog == NULL) {
            return -1;
        }

        dialog->hwnd = hwnd;

        hwnd_label = CreateWindowA("STATIC",
                                   "Name:",
                                   WS_CHILD | WS_VISIBLE,
                                   14,
                                   18,
                                   60,
                                   18,
                                   hwnd,
                                   (HMENU)0,
                                   dialog->app->hInstance,
                                   NULL);
        helper_app_set_font(hwnd_label);

        dialog->hwnd_name = CreateWindowA("EDIT",
                                          dialog->result.name,
                                          WS_CHILD | WS_VISIBLE | WS_BORDER |
                                              WS_TABSTOP | ES_AUTOHSCROLL,
                                          88,
                                          14,
                                          260,
                                          22,
                                          hwnd,
                                          (HMENU)HELPER_DIALOG_EDIT_NAME,
                                          dialog->app->hInstance,
                                          NULL);
        helper_app_set_font(dialog->hwnd_name);
        SendMessageA(dialog->hwnd_name,
                     EM_SETLIMITTEXT,
                     HELPER_PHONEBOOK_NAME_LEN - 1U,
                     0);

        hwnd_label = CreateWindowA("STATIC",
                                   "Number:",
                                   WS_CHILD | WS_VISIBLE,
                                   14,
                                   50,
                                   60,
                                   18,
                                   hwnd,
                                   (HMENU)0,
                                   dialog->app->hInstance,
                                   NULL);
        helper_app_set_font(hwnd_label);

        dialog->hwnd_number = CreateWindowA("EDIT",
                                            dialog->result.number,
                                            WS_CHILD | WS_VISIBLE | WS_BORDER |
                                                WS_TABSTOP | ES_AUTOHSCROLL,
                                            88,
                                            46,
                                            260,
                                            22,
                                            hwnd,
                                            (HMENU)HELPER_DIALOG_EDIT_NUMBER,
                                            dialog->app->hInstance,
                                            NULL);
        helper_app_set_font(dialog->hwnd_number);
        SendMessageA(dialog->hwnd_number,
                     EM_SETLIMITTEXT,
                     HELPER_PHONEBOOK_NUMBER_LEN - 1U,
                     0);
        if (dialog->number_locked) {
            EnableWindow(dialog->hwnd_number, FALSE);
        }

        hwnd_label = CreateWindowA("STATIC",
                                   "Target:",
                                   WS_CHILD | WS_VISIBLE,
                                   14,
                                   82,
                                   60,
                                   18,
                                   hwnd,
                                   (HMENU)0,
                                   dialog->app->hInstance,
                                   NULL);
        helper_app_set_font(hwnd_label);

        dialog->hwnd_target = CreateWindowA("EDIT",
                                            dialog->result.target,
                                            WS_CHILD | WS_VISIBLE | WS_BORDER |
                                                WS_TABSTOP | ES_AUTOHSCROLL,
                                            88,
                                            78,
                                            260,
                                            22,
                                            hwnd,
                                            (HMENU)HELPER_DIALOG_EDIT_TARGET,
                                            dialog->app->hInstance,
                                            NULL);
        helper_app_set_font(dialog->hwnd_target);
        SendMessageA(dialog->hwnd_target,
                     EM_SETLIMITTEXT,
                     HELPER_PHONEBOOK_TARGET_LEN - 1U,
                     0);

        dialog->hwnd_raw_mode = CreateWindowA("BUTTON",
                                              "RAW mode",
                                              WS_CHILD | WS_VISIBLE |
                                                  WS_TABSTOP | BS_AUTOCHECKBOX,
                                              88,
                                              110,
                                              120,
                                              20,
                                              hwnd,
                                              (HMENU)HELPER_DIALOG_CHECK_RAW,
                                              dialog->app->hInstance,
                                              NULL);
        helper_app_set_font(dialog->hwnd_raw_mode);
        SendMessageA(dialog->hwnd_raw_mode,
                     BM_SETCHECK,
                     dialog->result.raw_mode ? BST_CHECKED : BST_UNCHECKED,
                     0);

        control = CreateWindowA("BUTTON",
                                "OK",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                188,
                                146,
                                74,
                                24,
                                hwnd,
                                (HMENU)IDOK,
                                dialog->app->hInstance,
                                NULL);
        helper_app_set_font(control);

        control = CreateWindowA("BUTTON",
                                "Cancel",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                274,
                                146,
                                74,
                                24,
                                hwnd,
                                (HMENU)IDCANCEL,
                                dialog->app->hInstance,
                                NULL);
        helper_app_set_font(control);

        SetFocus(dialog->hwnd_name);
        return 0;
    }

    case WM_COMMAND:
        if (dialog == NULL) {
            break;
        }

        switch (LOWORD(wParam)) {
        case IDOK:
            GetWindowTextA(dialog->hwnd_name,
                           dialog->result.name,
                           sizeof(dialog->result.name));
            GetWindowTextA(dialog->hwnd_number,
                           dialog->result.number,
                           sizeof(dialog->result.number));
            GetWindowTextA(dialog->hwnd_target,
                           dialog->result.target,
                           sizeof(dialog->result.target));
            dialog->result.raw_mode =
                (SendMessageA(dialog->hwnd_raw_mode, BM_GETCHECK, 0, 0) ==
                 BST_CHECKED) ? 1 : 0;
            dialog->accepted = 1;
            dialog->done = 1;
            DestroyWindow(hwnd);
            return 0;

        case IDCANCEL:
            dialog->accepted = 0;
            dialog->done = 1;
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (dialog != NULL) {
            dialog->accepted = 0;
            dialog->done = 1;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (dialog != NULL) {
            dialog->hwnd = NULL;
        }
        return 0;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

static LRESULT CALLBACK helper_main_window_proc(HWND hwnd,
                                                UINT message,
                                                WPARAM wParam,
                                                LPARAM lParam)
{
    HELPER_APP *app;

    app = g_helper_app;

    switch (message) {
    case WM_CREATE:
        if (app != NULL) {
            app->hwnd_main = hwnd;
        }
        helper_app_create_main_controls(app);
        return 0;

    case WM_COMMAND:
        if (app == NULL) {
            break;
        }

        switch (LOWORD(wParam)) {
        case HELPER_CTRL_LISTENER_APPLY:
            if (HIWORD(wParam) == BN_CLICKED) {
                helper_app_apply_listener_from_controls(app);
            }
            return 0;

        case HELPER_CTRL_PHONEBOOK_ADD:
            helper_app_handle_phonebook_add(app);
            return 0;

        case HELPER_CTRL_PHONEBOOK_EDIT:
            helper_app_handle_phonebook_edit(app);
            return 0;

        case HELPER_CTRL_PHONEBOOK_DELETE:
            helper_app_handle_phonebook_delete(app);
            return 0;

        case HELPER_MENU_TRAY_OPEN:
            helper_app_restore_window(app);
            return 0;

        case HELPER_MENU_TRAY_EXIT:
            helper_app_request_exit(app);
            return 0;
        }
        break;

    case WM_NOTIFY:
        if (app == NULL) {
            break;
        }

        if (((NMHDR *)lParam)->idFrom == HELPER_CTRL_PHONEBOOK_LIST &&
            ((UINT)((NMHDR *)lParam)->code) == (UINT)NM_DBLCLK) {
            helper_app_handle_phonebook_edit(app);
            return 0;
        }
        break;

    case WM_TIMER:
        if (app == NULL || wParam != HELPER_APP_TIMER_ID) {
            break;
        }

        helper_net_runtime_check_timeout(&app->net);
        if (!helper_app_check_runtime_error(app)) {
            helper_show_error(app->hwnd_main,
                              "Helper network runtime failed.",
                              GetLastError());
            helper_app_request_exit(app);
            return 0;
        }

        if (!helper_app_retry_deferred(app)) {
            helper_show_error(app->hwnd_main,
                              "Deferred network write failed.",
                              GetLastError());
            helper_app_request_exit(app);
            return 0;
        }

        if (!helper_app_check_runtime_error(app)) {
            helper_show_error(app->hwnd_main,
                              "Helper network runtime failed.",
                              GetLastError());
            helper_app_request_exit(app);
            return 0;
        }

        if (!helper_app_drain_ipc(app)) {
            helper_show_error(app->hwnd_main,
                              "IPC receive or protocol handling failed.",
                              GetLastError());
            helper_app_request_exit(app);
            return 0;
        }

        helper_app_drain_vxd_trace(app);

        if (!helper_app_check_runtime_error(app)) {
            helper_show_error(app->hwnd_main,
                              "Helper network runtime failed.",
                              GetLastError());
            helper_app_request_exit(app);
            return 0;
        }

        helper_app_update_status(app);
        return 0;

    case HELPER_APP_WM_TRAY:
        if (app == NULL) {
            break;
        }

        switch (lParam) {
        case WM_LBUTTONDBLCLK:
            helper_app_restore_window(app);
            return 0;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
        {
            HMENU menu;
            POINT pt;

            menu = CreatePopupMenu();
            if (menu != NULL) {
                AppendMenuA(menu, MF_STRING, HELPER_MENU_TRAY_OPEN, "Open");
                AppendMenuA(menu, MF_STRING, HELPER_MENU_TRAY_EXIT, "Exit");
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu,
                               TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                               pt.x,
                               pt.y,
                               0,
                               hwnd,
                               NULL);
                DestroyMenu(menu);
            }
            return 0;
        }
        }
        break;

    case WM_CLOSE:
        if (app != NULL && !app->exiting) {
            helper_app_hide_to_tray(app);
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance,
                   HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine,
                   int nCmdShow)
{
    HELPER_APP app;
    MSG msg;
    int exit_code;

    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    if (!helper_app_initialize(&app, hInstance)) {
        helper_show_error(NULL, "Failed to start helper.", GetLastError());
        helper_app_shutdown(&app);
        return 1;
    }

    exit_code = 0;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    helper_app_shutdown(&app);
    return exit_code;
}
