#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>
#include <string.h> // Added for string manipulation

// LFRFID Hardware libraries
#include <lib/lfrfid/lfrfid_worker.h>
#include <lib/lfrfid/protocols/lfrfid_protocols.h>
#include <toolbox/protocols/protocol_dict.h>

#define SETTINGS_PATH EXT_PATH("apps_data/h10301_writer/settings.dat")
#define WORKER_EV_SUCCESS (1 << 0)

typedef struct {
    uint32_t fc;
    uint32_t cn;
    char status_msg[32];  // Changed to a char array to support dynamic updating
    uint8_t edit_mode;    // 0 = FC, 1 = CN
    uint32_t step_size;   // 1, 10, 100, 1000, 10000
} GymBurnerApp;

// Defensive storage loading
void save_settings(GymBurnerApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(storage) {
        storage_common_mkdir(storage, EXT_PATH("apps_data/h10301_writer"));
        File* file = storage_file_alloc(storage);
        if(storage_file_open(file, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            storage_file_write(file, app, sizeof(uint32_t) * 2); // Only save the first two numbers (fc, cn)
        }
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
    }
}

void load_settings(GymBurnerApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(storage) {
        File* file = storage_file_alloc(storage);
        if(storage_file_open(file, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
            storage_file_read(file, app, sizeof(uint32_t) * 2);
        } else {
            app->fc = 0;
            app->cn = 0;
        }
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
    }
    strlcpy(app->status_msg, "Ready to burn.", sizeof(app->status_msg));
}

// Helper to isolate and highlight only the digits affected by the step size
static void draw_number_highlight(Canvas* canvas, const char* label, uint32_t value, uint32_t step, bool is_active, bool blink, uint8_t y) {
    char val_str[16];
    snprintf(val_str, sizeof(val_str), "%lu", (unsigned long)value);
    
    // If not the active line, draw normally and return
    if (!is_active) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%s", label, val_str);
        canvas_draw_str(canvas, 10, y, buf);
        return;
    }

    // Determine how many digits are being modified
    int val_len = strlen(val_str);
    int hl_len = 1;
    if(step >= 10) hl_len = 2;
    if(step >= 100) hl_len = 3;
    if(step >= 1000) hl_len = 4;
    if(step >= 10000) hl_len = 5;
    
    // Prevent highlighting more digits than exist (e.g. step 100 on value "5")
    if (hl_len > val_len) hl_len = val_len;
    int prefix_len = val_len - hl_len;
    
    // Build the unmodified prefix string (e.g., "Card: 375") avoiding strncat
    char prefix_str[32];
    if (prefix_len > 0) {
        snprintf(prefix_str, sizeof(prefix_str), "%s%.*s", label, prefix_len, val_str);
    } else {
        snprintf(prefix_str, sizeof(prefix_str), "%s", label);
    }
    
    // Build the isolated active digits (e.g., "22")
    char hl_str[16];
    snprintf(hl_str, sizeof(hl_str), "%s", val_str + prefix_len);
    
    char suffix_str[32];
    snprintf(suffix_str, sizeof(suffix_str), "  [Step: %lu]", (unsigned long)step);
    
    // Measure pixel widths to perfectly place the box and the following text
    uint16_t prefix_w = canvas_string_width(canvas, prefix_str);
    uint16_t hl_w = canvas_string_width(canvas, hl_str);
    
    // Render Prefix
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_str(canvas, 10, y, prefix_str);
    
    // Render Highlight Box & Inverted Digits
    if (blink) {
        canvas_draw_box(canvas, 10 + prefix_w - 1, y - 9, hl_w + 2, 12);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_str(canvas, 10 + prefix_w, y, hl_str);
    
    // Render Suffix
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_str(canvas, 10 + prefix_w + hl_w, y, suffix_str);
}

static void render_callback(Canvas* canvas, void* ctx) {
    GymBurnerApp* app = ctx;
    if(!app) return;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "H10301 Writer v1.1");
    
    canvas_set_font(canvas, FontSecondary);
    
    // UI Instructions
    canvas_draw_str(canvas, 2, 22, "L/R: Step   U/D: Edit Val");
    canvas_draw_str(canvas, 2, 33, "OK: Swap    Hold OK: Burn");
    
    char buffer[64]; // Increased buffer size to safely hold "Status: " + 32 char msg
    
    // Check tick for blinking effect (400ms cycle)
    bool blink = (furi_get_tick() / 400) % 2;
    
    // Draw the specific digit highlights using our new precision helper
    draw_number_highlight(canvas, "FC: ", app->fc, app->step_size, app->edit_mode == 0, blink, 44);
    draw_number_highlight(canvas, "Card: ", app->cn, app->step_size, app->edit_mode == 1, blink, 55);
    
    // Status Bar (now rendering the char array instead of const char pointer)
    snprintf(buffer, sizeof(buffer), "Status: %s", app->status_msg);
    canvas_draw_str(canvas, 2, 64, buffer);
}

static void input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* event_queue = ctx;
    if(event_queue) {
        furi_message_queue_put(event_queue, input_event, FuriWaitForever);
    }
}

// Timer callback to trigger periodic UI redraws for the blink effect
static void timer_callback(void* ctx) {
    ViewPort* view_port = ctx;
    view_port_update(view_port);
}

// Native callback fired by the worker when it automatically verifies a successful write
static void rfid_write_cb(LFRFIDWorkerWriteResult result, void* context) {
    FuriEventFlag* flags = context;
    if(result == LFRFIDWorkerWriteOK) {
        furi_event_flag_set(flags, WORKER_EV_SUCCESS);
    }
}

// Actual T5577 Hardware Write Function matching the native app, now accepts context
bool write_t5577_hid(GymBurnerApp* app, ViewPort* view_port) {
    bool success = false;
    
    // 1. Allocate protocol dictionary and worker
    ProtocolDict* dict = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
    LFRFIDWorker* worker = lfrfid_worker_alloc(dict);
    
    // 2. Dynamically fetch the required memory footprint
    size_t data_size = protocol_dict_get_data_size(dict, LFRFIDProtocolH10301);
    uint8_t* data = malloc(data_size);
    memset(data, 0, data_size); 
    
    // Format the payload bytes
    if (data_size >= 3) {
        data[0] = (uint8_t)(app->fc & 0xFF);
        data[1] = (uint8_t)((app->cn >> 8) & 0xFF);
        data[2] = (uint8_t)(app->cn & 0xFF);
    }
    
    protocol_dict_set_data(dict, LFRFIDProtocolH10301, data, data_size);
    
    // 3. Create synchronization flag
    FuriEventFlag* flags = furi_event_flag_alloc();
    
    // 4. Start the worker just like the native 125kHz app does. 
    // The worker will continuously scan, write, and natively read-back to verify.
    lfrfid_worker_start_thread(worker);
    lfrfid_worker_write_start(worker, LFRFIDProtocolH10301, rfid_write_cb, flags);
    
    // 10 Second Countdown loop! Wait 1 second at a time to update the UI
    for(int i = 10; i > 0; i--) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Hold card (%ds)...", i);
        view_port_update(view_port); // Force screen redraw with new time

        uint32_t wait_result = furi_event_flag_wait(flags, WORKER_EV_SUCCESS, FuriFlagWaitAny, 1000); // Wait 1 sec
        
        if(wait_result & WORKER_EV_SUCCESS) {
            success = true;
            break;
        }
    }
    
    // 5. Cleanup safely to prevent furi_check crashes
    lfrfid_worker_stop(worker);
    lfrfid_worker_stop_thread(worker);
    
    furi_event_flag_free(flags);
    lfrfid_worker_free(worker);
    protocol_dict_free(dict); 
    free(data);
    
    return success;
}

int32_t h10301_writer_app(void* p) {
    UNUSED(p);
    
    GymBurnerApp* app = malloc(sizeof(GymBurnerApp));
    if(!app) return -1;
    load_settings(app);
    
    // Initialize UI state
    app->edit_mode = 1; // Default to editing CN
    app->step_size = 1;

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    Gui* gui = furi_record_open(RECORD_GUI);
    ViewPort* view_port = view_port_alloc();
    NotificationApp* notif = furi_record_open(RECORD_NOTIFICATION);
    
    if(!gui || !view_port || !event_queue || !notif) {
        if(view_port) view_port_free(view_port);
        if(gui) furi_record_close(RECORD_GUI);
        if(notif) furi_record_close(RECORD_NOTIFICATION);
        if(event_queue) furi_message_queue_free(event_queue);
        free(app);
        return -1;
    }

    view_port_draw_callback_set(view_port, render_callback, app);
    view_port_input_callback_set(view_port, input_callback, event_queue);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    // Create a timer to trigger UI redraws periodically for the blinking effect
    FuriTimer* blink_timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, view_port);
    furi_timer_start(blink_timer, 400); // Trigger every 400ms

    InputEvent event;
    while(1) {
        if(furi_message_queue_get(event_queue, &event, FuriWaitForever) == FuriStatusOk) {
            
            // Auto-Save whenever the user presses Back to exit the app
            if(event.key == InputKeyBack) {
                save_settings(app);
                break;
            }

            // Handle D-Pad Navigation and Values
            if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
                if(event.key == InputKeyUp) {
                    if(app->edit_mode == 0) {
                        app->fc += app->step_size;
                        if(app->fc > 255) app->fc = 255; // 8-bit limit
                    } else {
                        app->cn += app->step_size;
                        if(app->cn > 65535) app->cn = 65535; // 16-bit limit
                    }
                    strlcpy(app->status_msg, "Editing...", sizeof(app->status_msg));
                }
                else if(event.key == InputKeyDown) {
                    if(app->edit_mode == 0) {
                        if(app->fc >= app->step_size) app->fc -= app->step_size;
                        else app->fc = 0;
                    } else {
                        if(app->cn >= app->step_size) app->cn -= app->step_size;
                        else app->cn = 0;
                    }
                    strlcpy(app->status_msg, "Editing...", sizeof(app->status_msg));
                }
                else if(event.key == InputKeyLeft) {
                    if(app->step_size < 10000) app->step_size *= 10;
                }
                else if(event.key == InputKeyRight) {
                    if(app->step_size > 1) app->step_size /= 10;
                }
                else if(event.key == InputKeyOk && event.type == InputTypeShort) {
                    app->edit_mode = (app->edit_mode == 0) ? 1 : 0; // Swap FC and CN
                    app->step_size = 1; // Reset step size when swapping
                }
            }
            
            // Handle Long Press OK to Burn
            if(event.key == InputKeyOk && event.type == InputTypeLong) {
                // The status updating logic has been moved into write_t5577_hid for the countdown
                
                if(write_t5577_hid(app, view_port)) {
                    strlcpy(app->status_msg, "Success! (+1)", sizeof(app->status_msg));
                    app->cn++; 
                    save_settings(app);
                    notification_message(notif, &sequence_success);
                } else {
                    strlcpy(app->status_msg, "Write Failed!", sizeof(app->status_msg));
                    notification_message(notif, &sequence_error);
                }
            }
            view_port_update(view_port); // Immediate redraw on input
        }
    }

    // Cleanup
    furi_timer_stop(blink_timer);
    furi_timer_free(blink_timer);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
    return 0;
}