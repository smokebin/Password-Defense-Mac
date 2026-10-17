// save_stub.mm — Full cfg:: stub implementations for macOS UI compilation
// TODO: replace with ported save.h/save.cpp
#include "save_stub.h"

namespace cfg {
    static int s_theme = 0; // 0 = dark

    // Theme
    int  get_theme()            { return s_theme; }
    void set_theme(int theme)   { s_theme = theme; }
    bool is_dark_theme()        { return s_theme == 0; }

    // Colors — sensible defaults
    ColorRGBA get_soft_container_dark()   { return { 36/255.0f, 35/255.0f, 39/255.0f, 1.0f }; }
    ColorRGBA get_soft_container_light()  { return { 0.97f, 0.97f, 0.97f, 1.0f }; }
    ColorRGBA get_card_bg_dark()          { return { 45/255.0f, 44/255.0f, 49/255.0f, 1.0f }; }
    ColorRGBA get_card_bg_light()         { return { 1.0f, 1.0f, 1.0f, 1.0f }; }
    ColorRGBA get_card_header_bg_dark()   { return { 50/255.0f, 49/255.0f, 55/255.0f, 1.0f }; }
    ColorRGBA get_card_header_bg_light()  { return { 0.95f, 0.95f, 0.95f, 1.0f }; }
    ColorRGBA get_card_body_bg_dark()     { return { 40/255.0f, 39/255.0f, 44/255.0f, 1.0f }; }
    ColorRGBA get_card_body_bg_light()    { return { 0.98f, 0.98f, 0.98f, 1.0f }; }
    ColorRGBA get_controls_bg_dark()      { return { 30/255.0f, 29/255.0f, 33/255.0f, 1.0f }; }
    ColorRGBA get_controls_bg_light()     { return { 0.94f, 0.94f, 0.94f, 1.0f }; }
    ColorRGBA get_window_bg_dark()        { return { 25/255.0f, 24/255.0f, 28/255.0f, 1.0f }; }
    ColorRGBA get_window_bg_light()       { return { 0.92f, 0.92f, 0.92f, 1.0f }; }

    void set_soft_container_dark(float, float, float, float)   {}
    void set_soft_container_light(float, float, float, float)  {}
    void set_card_bg_dark(float, float, float, float)          {}
    void set_card_bg_light(float, float, float, float)         {}
    void set_card_header_bg_dark(float, float, float, float)   {}
    void set_card_header_bg_light(float, float, float, float)  {}
    void set_card_body_bg_dark(float, float, float, float)     {}
    void set_card_body_bg_light(float, float, float, float)    {}
    void set_controls_bg_dark(float, float, float, float)      {}
    void set_controls_bg_light(float, float, float, float)     {}
    void set_window_bg_dark(float, float, float, float)        {}
    void set_window_bg_light(float, float, float, float)       {}

    // View & sorting
    // s_view_mode default = 3 (ViewMode::ThreePane) for first-run consistency.
    static int s_view_mode = 3, s_order_key = 0, s_order_dir = 0, s_group_mode = 0;
    int  get_view_mode()          { return s_view_mode; }
    void set_view_mode(int mode)  { s_view_mode = mode; }
    int  get_order_key()          { return s_order_key; }
    void set_order_key(int key)   { s_order_key = key; }
    int  get_order_dir()          { return s_order_dir; }
    void set_order_dir(int dir)   { s_order_dir = dir; }
    int  get_group_mode()         { return s_group_mode; }
    void set_group_mode(int mode) { s_group_mode = mode; }

    // Pill tabs
    static std::string s_pill0 = "All", s_pill1 = "Favorites";
    std::string get_pill_tab_0()                       { return s_pill0; }
    void set_pill_tab_0(const std::string& label)      { s_pill0 = label; }
    std::string get_pill_tab_1()                       { return s_pill1; }
    void set_pill_tab_1(const std::string& label)      { s_pill1 = label; }

    // Behavior
    static bool s_hover_expand = true, s_show_group_count = true;
    static int  s_row_gap = 4;
    static bool s_autosave = true, s_autoscroll = true;
    static bool s_always_on_top = false, s_minimize_tray = false;
    static bool s_start_boot = false, s_start_minimized = false, s_auto_open_vault = false;

    bool get_hover_expand()              { return s_hover_expand; }
    void set_hover_expand(bool e)        { s_hover_expand = e; }
    bool get_show_group_count()          { return s_show_group_count; }
    void set_show_group_count(bool e)    { s_show_group_count = e; }
    int  get_row_gap()                   { return s_row_gap; }
    void set_row_gap(int gap)            { s_row_gap = gap; }
    bool get_autosave_enabled()          { return s_autosave; }
    void set_autosave_enabled(bool e)    { s_autosave = e; }
    bool get_autoscroll_enabled()        { return s_autoscroll; }
    void set_autoscroll_enabled(bool e)  { s_autoscroll = e; }
    bool get_always_on_top()             { return s_always_on_top; }
    void set_always_on_top(bool e)       { s_always_on_top = e; }
    bool get_minimize_to_tray()          { return s_minimize_tray; }
    void set_minimize_to_tray(bool e)    { s_minimize_tray = e; }
    bool get_start_on_boot()             { return s_start_boot; }
    void set_start_on_boot(bool e)       { s_start_boot = e; }
    bool get_start_minimized()           { return s_start_minimized; }
    void set_start_minimized(bool e)     { s_start_minimized = e; }
    bool get_auto_open_vault()           { return s_auto_open_vault; }
    void set_auto_open_vault(bool e)     { s_auto_open_vault = e; }

    // Backup
    static bool s_auto_backup = false;
    static int  s_backup_keep = 5;
    bool get_auto_backup()               { return s_auto_backup; }
    void set_auto_backup(bool e)         { s_auto_backup = e; }
    int  get_backup_keep_count()         { return s_backup_keep; }
    void set_backup_keep_count(int c)    { s_backup_keep = c; }

    // Security
    static int  s_clip_delay = 30, s_lock_timeout = 300;
    static bool s_reprompt_pw = true, s_reprompt_export = true;
    static bool s_reprompt_readonly = false, s_reprompt_notes = true;
    static int  s_reprompt_lockout = 3, s_self_destruct = 0;
    static int  s_pw_max_age = 90, s_trash_retention = 30;

    int  get_clipboard_clear_delay()          { return s_clip_delay; }
    void set_clipboard_clear_delay(int s)     { s_clip_delay = s; }
    int  get_auto_lock_timeout()              { return s_lock_timeout; }
    void set_auto_lock_timeout(int s)         { s_lock_timeout = s; }
    bool get_reprompt_reveal_password()        { return s_reprompt_pw; }
    void set_reprompt_reveal_password(bool e)  { s_reprompt_pw = e; }
    bool get_reprompt_export()                 { return s_reprompt_export; }
    void set_reprompt_export(bool e)           { s_reprompt_export = e; }
    bool get_reprompt_disable_readonly()       { return s_reprompt_readonly; }
    void set_reprompt_disable_readonly(bool e) { s_reprompt_readonly = e; }
    bool get_reprompt_reveal_notes()           { return s_reprompt_notes; }
    void set_reprompt_reveal_notes(bool e)     { s_reprompt_notes = e; }
    int  get_reprompt_lockout_count()          { return s_reprompt_lockout; }
    void set_reprompt_lockout_count(int c)     { s_reprompt_lockout = c; }
    int  get_self_destruct_mode()              { return s_self_destruct; }
    void set_self_destruct_mode(int m)         { s_self_destruct = m; }
    int  get_password_max_age_days()           { return s_pw_max_age; }
    void set_password_max_age_days(int d)      { s_pw_max_age = d; }
    int  get_trash_retention_days()            { return s_trash_retention; }
    void set_trash_retention_days(int d)       { s_trash_retention = d; }

    // Font / privacy
    static float s_font_scale = 1.0f;
    static bool  s_privacy = false;
    float get_font_scale()           { return s_font_scale; }
    void  set_font_scale(float s)    { s_font_scale = s; }
    bool  get_privacy_mode()         { return s_privacy; }
    void  set_privacy_mode(bool e)   { s_privacy = e; }
    static bool s_blur_unfocus = false;
    bool  get_blur_on_unfocus()      { return s_blur_unfocus; }
    void  set_blur_on_unfocus(bool e){ s_blur_unfocus = e; }

    // Recently used credentials
    static std::string s_recent_json = "[]";
    std::string get_recent_items_json()                    { return s_recent_json; }
    void set_recent_items_json(const std::string& json)    { s_recent_json = json; }

    // Detailed header columns
    static DetailedHeaderColumns s_dhc;
    DetailedHeaderColumns get_detailed_header_columns()                    { return s_dhc; }
    void set_detailed_header_columns(const DetailedHeaderColumns& cols)    { s_dhc = cols; }

    // Local server
    static bool s_local_server = false;
    static int  s_local_port = 8080;
    bool get_local_server_enabled()        { return s_local_server; }
    void set_local_server_enabled(bool e)  { s_local_server = e; }
    int  get_local_server_port()           { return s_local_port; }
    void set_local_server_port(int p)      { s_local_port = p; }

    // Three-pane
    static bool s_3p_sidebar_collapsed = false, s_3p_list_collapsed = false;
    static bool s_3p_sidebar_auto = true, s_3p_list_auto = true;
    bool get_three_pane_sidebar_collapsed()        { return s_3p_sidebar_collapsed; }
    void set_three_pane_sidebar_collapsed(bool c)  { s_3p_sidebar_collapsed = c; }
    bool get_three_pane_list_collapsed()            { return s_3p_list_collapsed; }
    void set_three_pane_list_collapsed(bool c)      { s_3p_list_collapsed = c; }
    bool get_three_pane_sidebar_auto_collapse()     { return s_3p_sidebar_auto; }
    void set_three_pane_sidebar_auto_collapse(bool e) { s_3p_sidebar_auto = e; }
    bool get_three_pane_list_auto_collapse()         { return s_3p_list_auto; }
    void set_three_pane_list_auto_collapse(bool e)   { s_3p_list_auto = e; }
}
