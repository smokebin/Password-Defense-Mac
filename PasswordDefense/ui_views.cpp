// ui_views.cpp
// View renderers: accordion items, tiles, three-pane, table, row state management
#include "ui_internal.h"

namespace ui
{
    // =========================================================
    // EditBuffers member functions (struct defined in ui_internal.h)
    // =========================================================
    void EditBuffers::BeginFrom(const AccordionItem& c, const std::string& password_value)
    {
        active = true;
        title = c.title; email = c.email; user = c.user;
        website = c.website; group = c.group; notes = c.notes;
        password = password_value;
        card_number = FormatWithPattern(StripNonDigits(c.card_number), "#### #### #### ####");
        card_expiry = FormatWithPattern(StripNonDigits(c.card_expiry), "## / ##");
        card_cvv = c.card_cvv; card_brand = c.card_brand;
        cardholder_name = c.cardholder_name;
        card_address = c.card_address;
        card_city = c.card_city; card_postal_code = c.card_postal_code;
        full_name = c.full_name; id_type_buf = c.id_type;
        id_number = c.id_number;
        date_of_birth = FormatWithPattern(StripNonDigits(c.date_of_birth), "## / ## / ####");
        expiry_date_buf = FormatWithPattern(StripNonDigits(c.expiry_date), "## / ## / ####");
        country = c.country; address = c.address;
        phone = FormatWithPattern(StripNonDigits(c.phone), "(###) ###-####");
        o_title = title; o_email = email; o_user = user;
        o_password = password;
        o_website = website; o_group = group; o_notes = notes;
        o_card_number = card_number; o_card_expiry = card_expiry;
        o_card_cvv = card_cvv; o_card_brand = card_brand;
        o_cardholder_name = cardholder_name;
        o_card_address = card_address;
        o_card_city = card_city; o_card_postal_code = card_postal_code;
        o_full_name = full_name; o_id_type_buf = id_type_buf;
        o_id_number = id_number; o_date_of_birth = date_of_birth;
        o_expiry_date_buf = expiry_date_buf; o_country = country;
        o_address = address; o_phone = phone;
        show_password = false;
    }

    void EditBuffers::Revert()
    {
        title = o_title; email = o_email; user = o_user;
        password = o_password;
        website = o_website; group = o_group; notes = o_notes;
        card_number = o_card_number; card_expiry = o_card_expiry;
        card_cvv = o_card_cvv; card_brand = o_card_brand;
        cardholder_name = o_cardholder_name;
        card_address = o_card_address;
        card_city = o_card_city; card_postal_code = o_card_postal_code;
        full_name = o_full_name; id_type_buf = o_id_type_buf;
        id_number = o_id_number; date_of_birth = o_date_of_birth;
        expiry_date_buf = o_expiry_date_buf; country = o_country;
        address = o_address; phone = o_phone;
        show_password = false;
    }

    // =========================================================
    // View state variable definitions (extern in ui_internal.h)
    // =========================================================
    std::unordered_map<uint64_t, EditBuffers> g_edit;
    std::unordered_set<uint64_t> g_open;
    std::unordered_map<uint64_t, int> g_scroll_to_open;
    std::unordered_map<uint64_t, TileCardState> g_tile_state;
    std::unordered_map<uint64_t, float> g_copy_flash;
    std::unordered_set<uint64_t> g_hover_opened;
    std::unordered_set<uint64_t> g_hover_flipped;
    bool  g_3p_sidebar_hover_expanded = false;
    bool  g_3p_list_hover_expanded = false;
    float g_3p_sidebar_leave_timer = 0.0f;
    float g_3p_list_leave_timer = 0.0f;
    float g_3p_sidebar_anim_w = -1.0f;
    float g_3p_list_anim_w    = -1.0f;
    std::unordered_set<uint64_t> g_notes_visible;
    uint64_t g_notes_reprompt_pending = 0;
    int g_notes_tab_reprompt_cred_id = -1;  // credential ID awaiting reprompt for notes tab switch
    std::unordered_map<uint64_t, float> g_card_body_h;
    std::unordered_map<uint64_t, float> g_card_anim_h;
    std::unordered_set<uint64_t>        g_card_closing;
    std::unordered_set<std::string>     g_collapsed_headers; // collapsed group headers in list views

    static inline bool GroupMatches(const AccordionItem& c, const std::string& group)
    {
        if (group == "Filter" || group == "All") return true;
        return c.group == group;
    }

    // Resolve what pill tab 0/1 actually means (could be customized)
    static inline bool PillTabMatches(const AccordionItem& c, const std::string& tabLabel)
    {
        if (tabLabel == "Pinned")    return c.is_pinned;
        if (tabLabel == "Favorites") return c.is_favorite;
        if (tabLabel == "Recent")    return true; // recent filtering handled elsewhere
        // Custom group name
        return c.group == tabLabel;
    }

    static inline bool SortModeMatches(const AccordionItem& c, int sort_mode, const std::string& tab0, const std::string& tab1)
    {
        if (sort_mode == 0) return PillTabMatches(c, tab0);
        if (sort_mode == 1) return PillTabMatches(c, tab1);
        return true; // All (2) or Recent (3)
    }

    // Get icon pair for a pill tab label
    static inline void GetPillTabIcons(const std::string& label, const char*& icon, const char*& iconActive)
    {
        if (label == "Pinned")         { icon = ICON_MDI_PIN;   iconActive = ICON_MDI_PIN; }
        else if (label == "Favorites") { icon = ICON_MDI_HEART; iconActive = ICON_MDI_HEART; }
        else if (label == "Recent")    { icon = ICON_MDI_HISTORY;       iconActive = ICON_MDI_HISTORY; }
        else                           { icon = ICON_MDI_FOLDER; iconActive = ICON_MDI_FOLDER; }
    }

    struct AccordionItemAction
    {
        int id = -1;
        bool requested_delete = false;
        bool edit_committed = false;
        bool edit_open_requested = false;
        // When set (>= 0) together with edit_open_requested, the dispatcher
        // opens THIS cred id instead of the popup's own id. Used by the
        // Security tab's "Also used by" list to jump to a sibling cred.
        int  edit_open_id_override = -1;
        bool anon_share_requested = false;
        bool toggle_pin_requested = false;
        bool toggle_fav_requested = false;
        AccordionItem edited{};
        std::string edited_password;
        bool selection_toggled = false;
        bool selected_now = false;
        bool interacted = false;
    };

    // 1) Row key helper
    uint64_t MakeRowKey(uint32_t activeVaultKey, int credId)
    {
        return (uint64_t(activeVaultKey) << 32) | uint32_t(credId);
    }

    uint64_t MakeCopyKey(uint64_t rowKey, int fieldIndex)
    {
        return rowKey ^ (uint64_t(fieldIndex) << 48);
    }

    static std::string FormatIsoDateLabel(const std::string& iso)
    {
        if (iso.size() < 10) return iso;
        const int y = std::atoi(iso.substr(0, 4).c_str());
        const int m = std::atoi(iso.substr(5, 2).c_str());
        const int d = std::atoi(iso.substr(8, 2).c_str());
        if (y <= 0 || m <= 0 || d <= 0) return iso;

        static const char* months[] = {
            "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec"
        };

        if (m < 1 || m > 12) return iso;

        char buf[32];
        snprintf(buf, sizeof(buf), "%s %d, %d", months[m - 1], d, y);
        return std::string(buf);
    }

    // Format Unix milliseconds to "Dec 30, 2025" display format
    static std::string FormatUnixMsDateLabel(int64_t ms)
    {
        if (ms == 0) return "";

        std::time_t sec = static_cast<std::time_t>(ms / 1000);
        std::tm tm_local;
#ifdef _WIN32
        localtime_s(&tm_local, &sec);
#else
        localtime_r(&sec, &tm_local);
#endif

        int y = tm_local.tm_year + 1900;
        int m = tm_local.tm_mon + 1;
        int d = tm_local.tm_mday;

        if (y <= 0 || m <= 0 || d <= 0) return "";

        static const char* months[] = {
            "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec"
        };

        if (m < 1 || m > 12) return "";

        int hour = tm_local.tm_hour;
        int min = tm_local.tm_min;

        char buf[48];
        snprintf(buf, sizeof(buf), "%s %d, %d at %02d:%02d", months[m - 1], d, y, hour, min);
        return std::string(buf);
    }

    // Format Unix milliseconds to "Dec 30, 2025" display format (date only, no time)
    static std::string FormatUnixMsDateOnly(int64_t ms)
    {
        if (ms == 0) return "";

        std::time_t sec = static_cast<std::time_t>(ms / 1000);
        std::tm tm_local;
#ifdef _WIN32
        localtime_s(&tm_local, &sec);
#else
        localtime_r(&sec, &tm_local);
#endif

        int y = tm_local.tm_year + 1900;
        int m = tm_local.tm_mon + 1;
        int d = tm_local.tm_mday;

        if (y <= 0 || m <= 0 || d <= 0) return "";

        static const char* months[] = {
            "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec"
        };

        if (m < 1 || m > 12) return "";

        char buf[32];
        snprintf(buf, sizeof(buf), "%s %d, %d", months[m - 1], d, y);
        return std::string(buf);
    }
    // Forward declaration (defined later, after row helpers).
    // Renders the password security analysis card. Navigation to a sibling
    // credential (via the "Reused across N other" link list) is handled
    // internally: three-pane mode sets s.three_pane_selected_id; other modes
    // open the target accordion row via g_open + g_scroll_to_open.
    static void DrawSecurityCard(const AccordionItem& c, ShellState& s,
        uint32_t activeVaultKey, GetPasswordFn get_password_fn, float contentW);

    // Share status row: "Shared · expires in Xh · 2/5 views"
    static void DrawShareStatusRow(const AccordionItem& c, ShellState& s)
    {
        if (!g_shell_ptr || c.uuid.empty()) return;

        // Check if this credential has a cached share status
        auto it = s.share_status_cache.find(c.uuid);

        // Request a fetch if we don't have cached data yet (or it's stale)
        bool need_request = false;
        if (it == s.share_status_cache.end()) {
            need_request = true;
        } else {
            int64_t age_ms = helpers::now_unix_ms() - it->second.fetched_at_ms;
            if (age_ms >= 60000) need_request = true;
        }

        if (need_request && !s.share_status_fetching.load(std::memory_order_relaxed))
        {
            // Avoid duplicate entries in the queue
            bool already_queued = false;
            for (const auto& q : s.share_status_request_queue)
                if (q == c.uuid) { already_queued = true; break; }
            if (!already_queued)
                s.share_status_request_queue.push_back(c.uuid);
        }

        if (it == s.share_status_cache.end() || !it->second.status.valid)
            return;

        const auto& st = it->second.status;

        // Build display string
        std::string line = ICON_MDI_SHARE_VARIANT " Shared";

        if (st.expired || st.views_exhausted) {
            line += "  \xc2\xb7  Expired";
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(174, 80, 80, 200));
            ImGui::TextWrapped("%s", line.c_str());
            ImGui::PopStyleColor();
            return;
        }

        // Time remaining
        if (st.expires_at_ms > 0) {
            int64_t remain_ms = st.expires_at_ms - helpers::now_unix_ms();
            if (remain_ms > 0) {
                int64_t remain_min = remain_ms / 60000;
                std::string time_str;
                if (remain_min < 60)
                    time_str = std::to_string(remain_min) + "m";
                else if (remain_min < 1440)
                    time_str = std::to_string(remain_min / 60) + "h";
                else
                    time_str = std::to_string(remain_min / 1440) + "d";
                line += "  \xc2\xb7  expires in " + time_str;
            }
        }

        // View count
        if (st.max_views > 0)
            line += "  \xc2\xb7  " + std::to_string(st.view_count) + "/" + std::to_string(st.max_views) + " views";
        else
            line += "  \xc2\xb7  " + std::to_string(st.view_count) + " views";

        ImGui::TextDisabled("%s", line.c_str());
    }

    // ============================================================
    // INFO ROW HELPERS (for accordion card body)
    // ============================================================

    // Clean URL for display (remove protocol and www)
    static std::string CleanUrl(const std::string& url)
    {
        std::string clean = url;
        if (clean.rfind("https://", 0) == 0) clean = clean.substr(8);
        else if (clean.rfind("http://", 0) == 0) clean = clean.substr(7);
        if (clean.rfind("www.", 0) == 0) clean = clean.substr(4);
        return clean;
    }

    // =========================================================
    // PRIVACY MODE — mask sensitive fields with bullets
    // =========================================================
    static const char* MaskIfPrivate(const std::string& text)
    {
        if (!g_shell_ptr || !g_shell_ptr->privacy_mode || text.empty())
            return nullptr;
        return "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2";
    }

    // =========================================================
    // FIELD CHANGE HIGHLIGHT (dim background on changed fields)
    // =========================================================
    static bool g_field_accent_active = false;
    static const ImU32 kFieldHighlight = IM_COL32(255, 98, 4, 18); // subtle orange wash

    static void DrawFieldHighlight(ImDrawList* dl, float rowX, float rowY, float rowW, float rowH)
    {
        if (!g_field_accent_active) return;
        dl->AddRectFilled(
            ImVec2(rowX, rowY),
            ImVec2(rowX + rowW, rowY + rowH),
            kFieldHighlight, 2.0f);
    }

    // ---- Copy-flash icon effect ----
    static std::unordered_map<ImGuiID, double> g_copy_flash_time;
    static constexpr double kCopyFlashDuration = 0.55; // seconds

    static void TriggerCopyFlash()
    {
        ImGuiID id = ImGui::GetID("##copy_flash");
        g_copy_flash_time[id] = ImGui::GetTime();
    }

    // Draw icon with optional copy-flash: green icon with darker outline
    static void DrawRowIcon(const char* icon)
    {
        ImGuiID id = ImGui::GetID("##copy_flash");
        double now = ImGui::GetTime();
        auto it = g_copy_flash_time.find(id);
        float flash = 0.0f;
        if (it != g_copy_flash_time.end())
        {
            double elapsed = now - it->second;
            if (elapsed < kCopyFlashDuration)
                flash = 1.0f - (float)(elapsed / kCopyFlashDuration);
            else
                g_copy_flash_time.erase(it);
        }

        if (flash > 0.01f)
        {
            // Lerp from bright green back to TextDisabled
            ImVec4 green(0.39f, 0.90f, 0.47f, 1.0f);
            ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ImVec4 col = ImLerp(dim, green, flash);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(icon);
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::TextDisabled(icon);
        }
    }

    // Info row: [Icon] text (no button, for group etc.)
    static void DrawInfoRow(
        const char* id,
        const char* icon,
        const std::string& value,
        float iconColW,
        float valueColW,
        float rowSpacing)
    {
        ImGui::PushID(id);
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = ImGui::GetTextLineHeight() + rowSpacing;
            float w = iconColW + valueColW;
            DrawFieldHighlight(ImGui::GetWindowDrawList(), p.x, p.y, w, h);
        }
        ImGui::TextDisabled(icon);
        ImGui::SameLine(iconColW);
        const char* display = value.empty() ? "—" : value.c_str();
        TextEllipsisClipped(display, valueColW);
        ImGui::Dummy(ImVec2(0, rowSpacing));
        ImGui::PopID();
    }

    // Floating icon button — no background, no bevel, just icon
    static bool IconFloatingBtn(const char* id, const char* icon, const char* tip, float sz)
    {
        ImGui::PushID(id);

        bool pressed = ImGui::InvisibleButton("##fl", ImVec2(sz, sz));
        bool hovered = ImGui::IsItemHovered();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pmin = ImGui::GetItemRectMin();
        ImVec2 pmax = ImGui::GetItemRectMax();
        ImVec2 center((pmin.x + pmax.x) * 0.5f, (pmin.y + pmax.y) * 0.5f);

        ImU32 iconCol = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        ImVec2 ts = ImGui::CalcTextSize(icon);
        dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), iconCol, icon);

        if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary))
        {
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
            SetTooltipPadded("%s", tip);
            ImGui::PopStyleColor();
        }

        ImGui::PopID();
        return pressed;
    }

    // Website row: [Globe] website.com [Launch] (launch on hover)
    static void DrawWebsiteRow(
        const char* id,
        const std::string& website,
        float iconColW,
        float valueColW,
        float rowWidth,
        float btnSz,
        float rowSpacing)
    {
        ImGui::PushID(id);

        ImVec2 startPos = ImGui::GetCursorScreenPos();
        float rowY = startPos.y;
        float rowH = ImGui::GetTextLineHeight() + rowSpacing;
        ImVec2 rowMin(ImGui::GetWindowPos().x, rowY);
        ImVec2 rowMax(rowMin.x + rowWidth, rowY + rowH);

        DrawFieldHighlight(ImGui::GetWindowDrawList(), rowMin.x, rowY, rowWidth, rowH);

        ImVec2 mp = ImGui::GetMousePos();
        bool anyModal = ImGui::IsPopupOpen((const char*)NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        bool rowHovered = !anyModal && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows) && (mp.x >= rowMin.x && mp.x <= rowMax.x && mp.y >= rowMin.y && mp.y <= rowMax.y);

        // Hover font scale
        if (rowHovered) ImGui::SetWindowFontScale(1.05f);

        // Click to launch (only in the text area, not the button zone)
        float btnZoneX = startPos.x + rowWidth - btnSz - 8.0f;
        if (rowHovered && !website.empty() && mp.x < btnZoneX && ImGui::IsMouseClicked(0))
        {
            system((std::string("open \"") + EnsureUrlProtocol(website) + "\"").c_str());
        }

        ImGui::TextDisabled(ICON_MDI_EARTH);
        ImGui::SameLine(iconColW);

        std::string display = website.empty() ? "—" : CleanUrl(website);
        TextEllipsisClipped(display.c_str(), valueColW);

        if (rowHovered) ImGui::SetWindowFontScale(1.0f);

        // Floating icon button (absolute positioned, no chrome)
        if (rowHovered && !website.empty())
        {
            ImVec2 btnPos(startPos.x + rowWidth - btnSz - 8.0f, rowY);
            ImGui::SetCursorScreenPos(btnPos);
            if (IconFloatingBtn("launch", ICON_MDI_OPEN_IN_NEW, "Launch", btnSz))
            {
                system((std::string("open \"") + EnsureUrlProtocol(website) + "\"").c_str());
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(startPos.x, rowY + ImGui::GetTextLineHeight()));
        ImGui::Dummy(ImVec2(0, rowSpacing));
        ImGui::PopID();
    }

    // Copy row: [Icon] value [Copy] (copy on hover)
    static void DrawCopyRow(
        const char* id,
        const char* icon,
        const std::string& value,
        float iconColW,
        float valueColW,
        float rowWidth,
        float btnSz,
        float rowSpacing,
        const char* display_override = nullptr)
    {
        ImGui::PushID(id);

        ImVec2 startPos = ImGui::GetCursorScreenPos();
        float rowY = startPos.y;
        float rowH = ImGui::GetTextLineHeight() + rowSpacing;
        ImVec2 rowMin(ImGui::GetWindowPos().x, rowY);
        ImVec2 rowMax(rowMin.x + rowWidth, rowY + rowH);

        DrawFieldHighlight(ImGui::GetWindowDrawList(), rowMin.x, rowY, rowWidth, rowH);

        ImVec2 mp = ImGui::GetMousePos();
        bool anyModal = ImGui::IsPopupOpen((const char*)NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        bool rowHovered = !anyModal && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows) && (mp.x >= rowMin.x && mp.x <= rowMax.x && mp.y >= rowMin.y && mp.y <= rowMax.y);

        // Hover font scale
        if (rowHovered) ImGui::SetWindowFontScale(1.05f);

        // Click to copy (only in the text area, not the button zone)
        float btnZoneX = startPos.x + rowWidth - btnSz - 8.0f;
        if (rowHovered && !value.empty() && mp.x < btnZoneX && ImGui::IsMouseClicked(0))
        {
            ImGui::SetClipboardText(value.c_str());
            TriggerCopyFlash();
            ShowToast("Copied", ToastType::Success);
        }

        DrawRowIcon(icon);
        ImGui::SameLine(iconColW);

        const char* display = value.empty() ? "\xe2\x80\x94"
                           : display_override ? display_override
                           : value.c_str();
        TextEllipsisClipped(display, valueColW);

        if (rowHovered) ImGui::SetWindowFontScale(1.0f);

        // Floating icon button (absolute positioned, no chrome)
        if (rowHovered && !value.empty())
        {
            ImVec2 btnPos(startPos.x + rowWidth - btnSz - 8.0f, rowY);
            ImGui::SetCursorScreenPos(btnPos);
            if (IconFloatingBtn("copy", ICON_MDI_CONTENT_COPY, "Copy", btnSz))
            {
                ImGui::SetClipboardText(value.c_str());
                TriggerCopyFlash();
                ShowToast("Copied", ToastType::Success);
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(startPos.x, rowY + ImGui::GetTextLineHeight()));
        ImGui::Dummy(ImVec2(0, rowSpacing));
        ImGui::PopID();
    }

    // Map email domain to webmail URL (returns empty if unknown)
    static std::string GetWebmailUrl(const std::string& email)
    {
        size_t at = email.rfind('@');
        if (at == std::string::npos || at + 1 >= email.size()) return "";

        std::string domain = email.substr(at + 1);
        // Lowercase for comparison
        for (auto& ch : domain) ch = (char)tolower((unsigned char)ch);

        if (domain == "gmail.com" || domain == "googlemail.com")
            return "https://mail.google.com";
        if (domain == "outlook.com" || domain == "hotmail.com" || domain == "live.com" || domain == "msn.com")
            return "https://outlook.live.com";
        if (domain == "yahoo.com" || domain == "ymail.com")
            return "https://mail.yahoo.com";
        if (domain == "protonmail.com" || domain == "proton.me" || domain == "pm.me")
            return "https://mail.proton.me";
        if (domain == "icloud.com" || domain == "me.com" || domain == "mac.com")
            return "https://www.icloud.com/mail";
        if (domain == "zoho.com")
            return "https://mail.zoho.com";
        if (domain == "aol.com")
            return "https://mail.aol.com";
        if (domain == "yandex.com" || domain == "yandex.ru")
            return "https://mail.yandex.com";
        if (domain == "tutanota.com" || domain == "tuta.io")
            return "https://app.tuta.com";
        if (domain == "gmx.com" || domain == "gmx.net")
            return "https://www.gmx.com";
        if (domain == "mail.com")
            return "https://www.mail.com";
        if (domain == "fastmail.com")
            return "https://app.fastmail.com";

        // Fallback: try https://mail.<domain>
        return "https://mail." + domain;
    }

    // Email row: [Email icon] email [Open Inbox | Copy] (on hover)
    static void DrawEmailRow(
        const char* id,
        const std::string& value,
        float iconColW,
        float valueColW,
        float rowWidth,
        float btnSz,
        float rowSpacing,
        const char* display_override = nullptr)
    {
        ImGui::PushID(id);

        ImVec2 startPos = ImGui::GetCursorScreenPos();
        float rowY = startPos.y;
        float rowH = ImGui::GetTextLineHeight() + rowSpacing;
        ImVec2 rowMin(ImGui::GetWindowPos().x, rowY);
        ImVec2 rowMax(rowMin.x + rowWidth, rowY + rowH);

        DrawFieldHighlight(ImGui::GetWindowDrawList(), rowMin.x, rowY, rowWidth, rowH);

        ImVec2 mp = ImGui::GetMousePos();
        bool anyModal = ImGui::IsPopupOpen((const char*)NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        bool rowHovered = !anyModal && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows) && (mp.x >= rowMin.x && mp.x <= rowMax.x && mp.y >= rowMin.y && mp.y <= rowMax.y);

        // Hover font scale
        if (rowHovered) ImGui::SetWindowFontScale(1.05f);

        // Click to copy (only in the text area, not the button zone)
        float btnZoneX = startPos.x + rowWidth - (btnSz + 4.0f) * 2 - 8.0f;
        if (rowHovered && !value.empty() && mp.x < btnZoneX && ImGui::IsMouseClicked(0))
        {
            ImGui::SetClipboardText(value.c_str());
            TriggerCopyFlash();
            ShowToast("Copied", ToastType::Success);
        }

        DrawRowIcon(ICON_MDI_EMAIL);
        ImGui::SameLine(iconColW);

        const char* display = value.empty() ? "\xe2\x80\x94"
                           : display_override ? display_override
                           : value.c_str();
        TextEllipsisClipped(display, valueColW);

        if (rowHovered) ImGui::SetWindowFontScale(1.0f);

        // Floating buttons on hover: [Open Inbox] [Copy]
        if (rowHovered && !value.empty())
        {
            std::string webmail = GetWebmailUrl(value);
            float rightX = startPos.x + rowWidth - 8.0f;

            // Copy button (rightmost)
            ImGui::SetCursorScreenPos(ImVec2(rightX - btnSz, rowY));
            if (IconFloatingBtn("copy", ICON_MDI_CONTENT_COPY, "Copy", btnSz))
            {
                ImGui::SetClipboardText(value.c_str());
                TriggerCopyFlash();
                ShowToast("Copied", ToastType::Success);
            }

            // Open inbox button (left of copy)
            if (!webmail.empty())
            {
                ImGui::SetCursorScreenPos(ImVec2(rightX - btnSz * 2 - 4.0f, rowY));
                if (IconFloatingBtn("inbox", ICON_MDI_OPEN_IN_NEW, "Open inbox", btnSz))
                {
                    system((std::string("open \"") + webmail + "\"").c_str());
                }
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(startPos.x, rowY + ImGui::GetTextLineHeight()));
        ImGui::Dummy(ImVec2(0, rowSpacing));
        ImGui::PopID();
    }

    // Masked copy row: shows masked value, reveals on hover
    static void DrawMaskedCopyRow(
        const char* id,
        const char* icon,
        const std::string& value,
        int reveal_last,        // show last N chars, rest masked (0 = fully masked)
        float iconColW,
        float valueColW,
        float rowWidth,
        float btnSz,
        float rowSpacing)
    {
        ImGui::PushID(id);

        ImVec2 startPos = ImGui::GetCursorScreenPos();
        float rowY = startPos.y;
        float rowH = ImGui::GetTextLineHeight() + rowSpacing;
        ImVec2 rowMin(ImGui::GetWindowPos().x, rowY);
        ImVec2 rowMax(rowMin.x + rowWidth, rowY + rowH);

        DrawFieldHighlight(ImGui::GetWindowDrawList(), rowMin.x, rowY, rowWidth, rowH);

        ImVec2 mp = ImGui::GetMousePos();
        bool anyModal = ImGui::IsPopupOpen((const char*)NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        bool rowHovered = !anyModal && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows) && (mp.x >= rowMin.x && mp.x <= rowMax.x && mp.y >= rowMin.y && mp.y <= rowMax.y);

        if (rowHovered) ImGui::SetWindowFontScale(1.05f);

        float btnZoneX = startPos.x + rowWidth - btnSz - 8.0f;
        if (rowHovered && !value.empty() && mp.x < btnZoneX && ImGui::IsMouseClicked(0))
        {
            ImGui::SetClipboardText(value.c_str());
            TriggerCopyFlash();
            ShowToast("Copied", ToastType::Success);
        }

        DrawRowIcon(icon);
        ImGui::SameLine(iconColW);

        // Build masked display
        std::string display;
        if (value.empty()) {
            display = "\xe2\x80\x94"; // em dash
        } else if (rowHovered) {
            display = value; // reveal on hover
        } else {
            int len = (int)value.size();
            int show = (reveal_last > 0 && reveal_last < len) ? reveal_last : 0;
            int hide = len - show;
            display.clear();
            for (int i = 0; i < hide; i++) display += "\xe2\x80\xa2"; // bullet
            if (show > 0) display += value.substr(len - show);
        }
        TextEllipsisClipped(display.c_str(), valueColW);

        if (rowHovered) ImGui::SetWindowFontScale(1.0f);

        // Floating icon button (absolute positioned, no chrome)
        if (rowHovered && !value.empty())
        {
            ImVec2 btnPos(startPos.x + rowWidth - btnSz - 8.0f, rowY);
            ImGui::SetCursorScreenPos(btnPos);
            if (IconFloatingBtn("copy", ICON_MDI_CONTENT_COPY, "Copy", btnSz))
            {
                ImGui::SetClipboardText(value.c_str());
                TriggerCopyFlash();
                ShowToast("Copied", ToastType::Success);
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(startPos.x, rowY + ImGui::GetTextLineHeight()));
        ImGui::Dummy(ImVec2(0, rowSpacing));
        ImGui::PopID();
    }

    void DrawBadgeChip(ImDrawList* dl, const ImVec2& bMin, const ImVec2& bMax, ImU32 bg, const char* text, ImU32 textCol)
    {
        dl->AddRectFilled(bMin, bMax, bg, 8.0f);
        dl->AddText(ImVec2(bMin.x + 6.0f, bMin.y + 1.0f), textCol, text);
    }

    // Draw security warning badges (weak/reused), returns updated rightX
    float DrawSecurityBadges(ImDrawList* dl, const ShellState* shell,
        int credId, float rightX, float centerY, float chipGap)
    {
        if (!shell) return rightX;

        auto draw_icon = [&](const char* icon, ImU32 col) {
            ImVec2 sz = ImGui::CalcTextSize(icon);
            rightX -= sz.x;
            dl->AddText(ImVec2(rightX, centerY), col, icon);
            rightX -= chipGap;
        };

        if (shell->sec_highlight_weak && shell->sec_weak_ids.count(credId))
            draw_icon(ICON_MDI_ALERT, IM_COL32(174, 41, 41, 255));

        if (shell->sec_highlight_reused && shell->sec_reused_ids.count(credId))
            draw_icon(ICON_MDI_REPEAT, IM_COL32(190, 156, 63, 255));

        if (shell->sec_highlight_exposed && shell->sec_exposed_ids.count(credId))
            draw_icon(ICON_MDI_EARTH, IM_COL32(142, 68, 173, 255));

        if (shell->sec_highlight_aging && shell->sec_aging_ids.count(credId))
            draw_icon(ICON_MDI_CLOCK_ALERT, IM_COL32(39, 174, 157, 255));

        return rightX;
    }

    bool IconSquareBtn(const char* id, const char* icon, const char* tip,
        float sz, bool active, bool danger, ImU32 activeIconCol)
    {
        ImGui::PushID(id);

        bool pressed = ImGui::InvisibleButton("##sq", ImVec2(sz, sz));
        bool hovered = ImGui::IsItemHovered();
        bool held    = ImGui::IsItemActive();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pmin = ImGui::GetItemRectMin();
        ImVec2 pmax = ImGui::GetItemRectMax();
        ImVec2 center((pmin.x + pmax.x) * 0.5f, (pmin.y + pmax.y) * 0.5f);
        const float rounding = 6.0f;
        const bool dark = IsDarkTheme();

        // SoftBevel chrome — same pattern as HoldToActionButton (ui_widgets.cpp:577+)
        // so pin/fav/share/edit visually match the Delete button's lift + highlight.
        const ImVec4 col_bg_v     = dark ? ImVec4(0.133f, 0.133f, 0.133f, 1.0f) : ImVec4(0.94f, 0.94f, 0.95f, 1.0f);
        const ImVec4 col_bg_hov_v = dark ? ImVec4(0.165f, 0.165f, 0.165f, 1.0f) : ImVec4(0.91f, 0.91f, 0.91f, 1.0f);
        const ImVec4 col_bg_act_v = dark ? ImVec4(0.102f, 0.102f, 0.102f, 1.0f) : ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
        const ImVec4 col_shadow   = dark ? ImVec4(0.0f, 0.0f, 0.0f, 0.392f)     : ImVec4(0.0f, 0.0f, 0.0f, 0.137f);
        const ImVec4 col_hl       = dark ? ImVec4(1.0f, 1.0f, 1.0f, 0.024f)     : ImVec4(1.0f, 1.0f, 1.0f, 0.549f);
        const ImVec4 col_hl_hov   = dark ? ImVec4(1.0f, 1.0f, 1.0f, 0.047f)     : ImVec4(1.0f, 1.0f, 1.0f, 0.784f);

        ImU32 bgCol     = ImGui::ColorConvertFloat4ToU32(held ? col_bg_act_v : (hovered ? col_bg_hov_v : col_bg_v));
        ImU32 shadowCol = ImGui::ColorConvertFloat4ToU32(col_shadow);

        // Splitter: lip shadows behind the body.
        ImDrawListSplitter splitter;
        splitter.Split(dl, 2);
        splitter.SetCurrentChannel(dl, 1);

        // Main body
        dl->AddRectFilled(pmin, pmax, bgCol, rounding);

        // Bottom lip shadow
        {
            const float lipOffset = 1.5f;
            float clipTop = pmin.y + (sz * 0.6f);
            splitter.SetCurrentChannel(dl, 0);
            dl->PushClipRect(
                ImVec2(pmin.x - 2.0f, clipTop),
                ImVec2(pmax.x + 2.0f, pmax.y + lipOffset + 2.0f), true);
            dl->AddRectFilled(
                ImVec2(pmin.x, pmin.y + lipOffset),
                ImVec2(pmax.x, pmax.y + lipOffset),
                shadowCol, rounding);
            dl->PopClipRect();
            splitter.SetCurrentChannel(dl, 1);
        }

        // Top highlight inset
        {
            ImVec4 hlV = hovered ? col_hl_hov : col_hl;
            const float inset = 1.0f;
            float clipBottom = pmin.y + (sz * 0.35f);
            float hlRound = rounding - inset;
            if (hlRound < 0.0f) hlRound = 0.0f;
            dl->PushClipRect(
                ImVec2(pmin.x + inset - 1.0f, pmin.y + inset - 1.0f),
                ImVec2(pmax.x - inset + 1.0f, clipBottom), true);
            dl->AddRect(
                ImVec2(pmin.x + inset, pmin.y + inset),
                ImVec2(pmax.x - inset, pmax.y - inset),
                ImGui::ColorConvertFloat4ToU32(hlV), hlRound, 0, 1.0f);
            dl->PopClipRect();
        }

        splitter.Merge(dl);

        // Icon centered
        ImU32 iconCol;
        if (danger)
            iconCol = IM_COL32(220, 80, 80, 255);
        else if (active && activeIconCol != 0)
            iconCol = activeIconCol;
        else
            iconCol = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);

        ImVec2 ts = ImGui::CalcTextSize(icon);
        dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), iconCol, icon);

        if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary))
        {
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
            SetTooltipPadded("%s", tip);
            ImGui::PopStyleColor();
        }

        ImGui::PopID();

        return pressed;
    }

    // Icon for Credential type
    const char* CredTypeIcon(CredType t)
    {
        switch (t) {
        case CredType::CreditCard: return ICON_MDI_CREDIT_CARD;
        case CredType::Identity:   return ICON_MDI_CARD_ACCOUNT_DETAILS;
        case CredType::SecureNote: return ICON_MDI_NOTE_TEXT;
        default:                   return ICON_MDI_KEY;
        }
    }

    // TOTP row: [Clock] 042 837 (23s) [Copy on hover]
    static void DrawTOTPRow(
        const char* id,
        const std::string& totp_secret,
        float iconColW,
        float valueColW,
        float rowWidth,
        float btnSz,
        float rowSpacing)
    {
        if (totp_secret.empty()) return;

        auto bytes = totp::base32_decode(totp_secret);
        if (bytes.size() < 10) return;

        std::string code = totp::generate_code_now(bytes);
        int secs = totp::seconds_remaining_now();

        // Format code with space in middle: "042 837"
        std::string display_code;
        if (code.size() == 6)
            display_code = code.substr(0, 3) + " " + code.substr(3);
        else
            display_code = code;

        char display[64];
        snprintf(display, sizeof(display), "%s  (%ds)", display_code.c_str(), secs);

        ImGui::PushID(id);

        ImVec2 startPos = ImGui::GetCursorScreenPos();
        float rowY = startPos.y;
        float rowH = ImGui::GetTextLineHeight() + rowSpacing;
        ImVec2 rowMin(ImGui::GetWindowPos().x, rowY);
        ImVec2 rowMax(rowMin.x + rowWidth, rowY + rowH);

        DrawFieldHighlight(ImGui::GetWindowDrawList(), rowMin.x, rowY, rowWidth, rowH);

        ImVec2 mp = ImGui::GetMousePos();
        bool anyModal = ImGui::IsPopupOpen((const char*)NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        bool rowHovered = !anyModal && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows) && (mp.x >= rowMin.x && mp.x <= rowMax.x && mp.y >= rowMin.y && mp.y <= rowMax.y);

        if (rowHovered) ImGui::SetWindowFontScale(1.05f);

        // Click to copy code
        float btnZoneX = startPos.x + rowWidth - btnSz - 8.0f;
        if (rowHovered && mp.x < btnZoneX && ImGui::IsMouseClicked(0))
        {
            ImGui::SetClipboardText(code.c_str());
            ShowToast("TOTP copied", ToastType::Success);
        }

        // Color: green when >5s, red when <=5s
        ImVec4 col = (secs > 5) ? colors::Green : colors::Red;

        ImGui::TextDisabled(ICON_MDI_CLOCK);
        ImGui::SameLine(iconColW);
        ImGui::TextColored(col, "%s", display);

        if (rowHovered) ImGui::SetWindowFontScale(1.0f);

        // Floating copy icon on hover
        if (rowHovered)
        {
            ImVec2 btnPos(startPos.x + rowWidth - btnSz - 8.0f, rowY);
            ImGui::SetCursorScreenPos(btnPos);
            if (IconFloatingBtn("copy_totp", ICON_MDI_CONTENT_COPY, "Copy TOTP", btnSz))
            {
                ImGui::SetClipboardText(code.c_str());
                ShowToast("TOTP copied", ToastType::Success);
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(startPos.x, rowY + ImGui::GetTextLineHeight()));
        ImGui::Dummy(ImVec2(0, rowSpacing));
        ImGui::PopID();
    }

    // Forward declaration for re-prompt pending Credential ID
    static int s_reprompt_pending_cred_id = -1;

    // Password row: [Key] •••••••• [Eye] [Copy] (buttons on hover)
    static void DrawPasswordRow(
        const char* id,
        int credId,
        GetPasswordFn get_password_fn,
        const std::vector<PwHistoryEntry>& password_history,
        std::set<int>& visiblePasswords,
        float iconColW,
        float valueColW,
        float rowWidth,
        float btnSz,
        float rowSpacing)
    {
        ImGui::PushID(id);

        ImVec2 startPos = ImGui::GetCursorScreenPos();
        float rowY = startPos.y;
        float rowH = ImGui::GetTextLineHeight() + rowSpacing;
        ImVec2 rowMin(ImGui::GetWindowPos().x, rowY);
        ImVec2 rowMax(rowMin.x + rowWidth, rowY + rowH);

        DrawFieldHighlight(ImGui::GetWindowDrawList(), rowMin.x, rowY, rowWidth, rowH);

        ImVec2 mp = ImGui::GetMousePos();
        bool anyModal = ImGui::IsPopupOpen((const char*)NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        bool rowHovered = !anyModal && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows) && (mp.x >= rowMin.x && mp.x <= rowMax.x && mp.y >= rowMin.y && mp.y <= rowMax.y);

        bool pwVisible = visiblePasswords.count(credId) > 0;
        const char* pw = get_password_fn ? get_password_fn(credId) : "";
        bool hasPw = pw && pw[0];

        // Hover font scale
        if (rowHovered) ImGui::SetWindowFontScale(1.05f);

        // Click to copy (only in the text area, not the button zone)
        float btnZoneX = startPos.x + rowWidth - btnSz * 2 - 12.0f;
        if (rowHovered && hasPw && mp.x < btnZoneX && ImGui::IsMouseClicked(0))
        {
            ClipboardCopyPassword(pw);
            TriggerCopyFlash();
        }

        DrawRowIcon(ICON_MDI_KEY);
        ImGui::SameLine(iconColW);

        std::string display = pwVisible && hasPw ? pw : "••••••••";
        TextEllipsisClipped(display.c_str(), valueColW);

        if (rowHovered) ImGui::SetWindowFontScale(1.0f);

        // Check if re-prompt was approved for this Credential
        if (IsRepromptApproved(RepromptAction::RevealPassword) && s_reprompt_pending_cred_id == credId)
        {
            visiblePasswords.insert(credId);
            ClearRepromptApproval(RepromptAction::RevealPassword);
            s_reprompt_pending_cred_id = -1;
        }

        // Floating icon buttons (absolute positioned, no chrome)
        if (rowHovered)
        {
            float btnX = startPos.x + rowWidth - btnSz * 2 - 12.0f;
            ImGui::SetCursorScreenPos(ImVec2(btnX, rowY));
            if (IconFloatingBtn("toggle", pwVisible ? ICON_MDI_EYE_OFF : ICON_MDI_EYE,
                pwVisible ? "Hide" : "Show", btnSz))
            {
                if (pwVisible)
                {
                    visiblePasswords.erase(credId);
                }
                else
                {
                    // Request re-prompt if enabled
                    s_reprompt_pending_cred_id = credId;
                    if (RequestReprompt(RepromptAction::RevealPassword, ""))
                    {
                        visiblePasswords.insert(credId);
                        s_reprompt_pending_cred_id = -1;
                    }
                }
            }

            if (hasPw)
            {
                ImGui::SetCursorScreenPos(ImVec2(btnX + btnSz + 4.0f, rowY));
                if (IconFloatingBtn("copy", ICON_MDI_CONTENT_COPY, "Copy", btnSz))
                {
                    ClipboardCopyPassword(pw);
                    TriggerCopyFlash();
                }
            }
        }

        // Right-click: password history popup (InvisibleButton for reliable hit-testing in child windows)
        if (!password_history.empty())
        {
            ImGui::SetCursorScreenPos(rowMin);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##pw_row_ctx", ImVec2(rowMax.x - rowMin.x, rowH));

            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,  8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(8, 6));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8, 6));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, IsDarkTheme() ? IM_COL32(26, 26, 28, 255) : IM_COL32(250, 250, 252, 255));
            ImGui::PushStyleColor(ImGuiCol_Border,  IsDarkTheme() ? IM_COL32(255,255,255,25) : IM_COL32(0,0,0,30));
            if (ImGui::BeginPopupContextItem("##pw_history_ctx"))
            {
                PopupStyleBegin();
                ImGui::TextDisabled(ICON_MDI_HISTORY " Password History");
                ImGui::Separator();

                for (int i = 0; i < (int)password_history.size(); i++)
                {
                    const auto& entry = password_history[i];
                    std::string dateStr = FormatUnixMsDateLabel(entry.changed_at_ms);

                    char label[256];
                    snprintf(label, sizeof(label), "%s  \xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2##hist_%d", dateStr.c_str(), i);

                    if (BarMenuItem(label, false))
                    {
                        ClipboardCopyPassword(entry.password.c_str());
                        ShowToast("Copied previous password", ToastType::Success);
                    }

                    if (ImGui::IsItemHovered())
                    {
                        SetTooltipPadded("%s", entry.password.c_str());
                    }
                }

                PopupStyleEnd();
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(5);
        }

        ImGui::SetCursorScreenPos(ImVec2(startPos.x, rowY + ImGui::GetTextLineHeight()));
        ImGui::Dummy(ImVec2(0, rowSpacing));
        ImGui::PopID();
    }

    // ============================================================
    // ACCORDION ITEM RENDERER (Clean information display)
    // ============================================================
    // Features:
    // - Clean card-based design with checkbox selection
    // - Title acts as header anchor with PIN/FAV chips
    // - Simple information display (no buttons in body)
    // - Proper padding inside card for better readability
    // - Metadata section with lighter styling
    // ============================================================

    AccordionItemAction RenderAccordionItem(
        const AccordionItem& c,
        uint32_t activeVaultKey,
        GetPasswordFn get_password_fn,
        bool read_only,
        int displayIndex = -1)
    {
        // ===========================================================
        // STRUCTURE:
        // 1. Checkbox (left gutter, shows on hover/selection)
        // 2. Card header (title + PIN/FAV chips, click to expand)
        // 3. Card body (when expanded):
        //    - Information fields with labels and values
        //    - Metadata (Created/Updated) with lighter styling
        // ===========================================================

        AccordionItemAction out{};
        out.id = c.id;

        const uint64_t rowKey = MakeRowKey(activeVaultKey, c.id);
        const bool sel = IsSelected(rowKey);
        const bool anySel = HasAnySelection(activeVaultKey);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Layout constants
        const float maxCardW = 900.0f;
        const float rawAvailW = ImGui::GetContentRegionAvail().x - 32.f;
        const float availW = ImMin(rawAvailW, maxCardW);
        const float rounding = 0.0f;
        const float headerH = 48.0f;
        const float padX = 12.0f;

        const float cbColW = 28.0f;  // Checkbox gutter width
        const float cbPadL = 6.0f;   // Checkbox left padding

        // Colors (theme-aware)
        const ImU32 colCard = ImGui::GetColorU32(GetCardHeaderBg());
        const ImU32 colBorder = ImGui::GetColorU32(colors::Trans);
        const ImU32 colAccent = ImGui::GetColorU32(colors::SecondColor);

        ImVec2 rowMin = ImGui::GetCursorScreenPos();

        // ===========================================================
        // CHECKBOX (left gutter)
        // ===========================================================
        ImRect cardRect(
            ImVec2(rowMin.x + cbColW, rowMin.y),
            ImVec2(rowMin.x + availW, rowMin.y + headerH)
        );

        ImGui::PushID((void*)(uintptr_t)rowKey);

        // Show checkbox on hover or when any selection exists
        ImVec2 mp = ImGui::GetMousePos();
        const bool anyPopup = ImGui::IsPopupOpen((const char*)NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) || (g_shell_ptr && g_shell_ptr->settings_modal_open);
        const bool rowHovered = !anyPopup && ImRect(rowMin, ImVec2(rowMin.x + availW, rowMin.y + headerH)).Contains(mp);
        const bool showCb = rowHovered || sel || anySel;

        if (showCb)
        {
            float cbY = rowMin.y + (headerH - 15.0f) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x + cbPadL, cbY));
            bool cbVal = sel;
            if (ImGui::Checkbox2("", &cbVal))
            {
                ToggleSelected(rowKey);
                out.selection_toggled = true;
                out.selected_now = IsSelected(rowKey);
                out.interacted = true;
            }
        }

        // Check if card is open (need to know for shadow height)
        const bool open = (g_open.count(rowKey) != 0);

        // ===========================================================
        // CARD SHADOW (multi-ring for subtle elevation)
        // ===========================================================
        // Note: Shadow is drawn for header only here; body shadow will be added when body is rendered
        // Draw 4 rings for a soft shadow effect
        const float shadowOffset = 2.0f;  // Slight downward offset
        const ImVec2 shadowStart = ImVec2(cardRect.Min.x, cardRect.Min.y + shadowOffset);

        // Ring 1 (outermost, most transparent)
        dl->AddRectFilled(
            ImVec2(shadowStart.x - 4.0f, shadowStart.y - 4.0f),
            ImVec2(cardRect.Max.x + 4.0f, cardRect.Max.y + 4.0f),
            GetShadowColor(0, rowHovered),
            rounding
        );

        // Ring 2
        dl->AddRectFilled(
            ImVec2(shadowStart.x - 2.0f, shadowStart.y - 2.0f),
            ImVec2(cardRect.Max.x + 2.0f, cardRect.Max.y + 2.0f),
            GetShadowColor(1, rowHovered),
            rounding
        );

        // Ring 3
        dl->AddRectFilled(
            ImVec2(shadowStart.x - 1.0f, shadowStart.y - 1.0f),
            ImVec2(cardRect.Max.x + 1.0f, cardRect.Max.y + 1.0f),
            GetShadowColor(2, rowHovered),
            rounding
        );

        // Ring 4 (innermost, most opaque)
        dl->AddRectFilled(
            shadowStart,
            cardRect.Max,
            GetShadowColor(3, rowHovered),
            rounding
        );

        // ===========================================================
        // CARD HEADER (background + click handler)
        // ===========================================================
        dl->AddRectFilled(cardRect.Min, cardRect.Max, colCard, rounding);
        if (c.is_changed)
            dl->AddRectFilled(cardRect.Min, cardRect.Max,
                (c.changed_fields & FCF_IsNew) ? kNewRowTint : kChangedRowTint, rounding);
        dl->AddRect(cardRect.Min, cardRect.Max, colBorder, rounding, 0, 1.0f);

        // Click to toggle expand
        ImGui::SetCursorScreenPos(cardRect.Min);
        ImGui::InvisibleButton("##acc_header_btn", cardRect.GetSize());

        if (ImGui::IsItemActivated())
        {
            g_hover_opened.erase(rowKey); // click-opened items persist
            if (g_open.count(rowKey)) {
                g_open.erase(rowKey);
                g_notes_visible.erase(rowKey);
                if (g_card_body_h.count(rowKey)) g_card_closing.insert(rowKey);
            }
            else {
                g_open.insert(rowKey);
                g_scroll_to_open[rowKey] = 2;  // Wait 2 frames for body to fully render
            }
            out.interacted = true;
        }

        // Hover-expand: open on header hover (close is deferred until after body render)
        if (g_shell_ptr && g_shell_ptr->hover_expand)
        {
            if (rowHovered && !g_open.count(rowKey))
            {
                g_open.insert(rowKey);
                g_hover_opened.insert(rowKey);
            }
        }

        // ===========================================================
        // TITLE + STATUS CHIPS (PIN/FAV)
        // ===========================================================
        const char* title = DisplayTitle(c.title);

        const float chipH = 18.0f;
        const float chipPadX = 6.0f;
        const float chipGap = 6.0f;

        float rightX = cardRect.Max.x - padX;

        // Draw chips from right to left
        auto draw_chip = [&](const char* label)
            {
                ImVec2 tsz = ImGui::CalcTextSize(label);
                float bw = tsz.x + chipPadX * 2.0f;
                float bh = chipH;

                rightX -= bw;
                ImVec2 bMin(rightX, cardRect.Min.y + (headerH - bh) * 0.5f);
                ImVec2 bMax(rightX + bw, bMin.y + bh);

                DrawBadgeChip(dl, bMin, bMax, colAccent, label, GetBadgeTextColor());
                rightX -= chipGap;
            };

        if (c.is_favorite)
        {
            const char* fav = ICON_MDI_HEART;
            ImVec2 sz = ImGui::CalcTextSize(fav);
            rightX -= sz.x;
            dl->AddText(ImVec2(rightX, cardRect.Min.y + (headerH - sz.y) * 0.5f),
                GetFavoriteColor(), fav);
            rightX -= chipGap;
        }
        if (c.is_pinned)
        {
            const char* pin = ICON_MDI_PIN;
            ImVec2 sz = ImGui::CalcTextSize(pin);
            rightX -= sz.x;
            dl->AddText(ImVec2(rightX, cardRect.Min.y + (headerH - sz.y) * 0.5f),
                ImGui::GetColorU32(colors::MainColor), pin);
            rightX -= chipGap;
        }
        // Timer badge
        if (c.expires_at_ms != 0)
        {
            if (c.expires_at_ms < 0) {
                // Expired + flagged
                const char* ico = ICON_MDI_TIMER_SAND_COMPLETE;
                ImVec2 sz = ImGui::CalcTextSize(ico);
                rightX -= sz.x;
                dl->AddText(ImVec2(rightX, cardRect.Min.y + (headerH - sz.y) * 0.5f),
                    IM_COL32(220, 50, 50, 255), ico);
                rightX -= chipGap;
            } else {
                // Active timer — show icon + remaining time
                int64_t remain_ms = c.expires_at_ms - helpers::now_unix_ms();
                if (remain_ms < 0) remain_ms = 0;
                char tbuf[32];
                if (remain_ms >= 86400000LL)
                    snprintf(tbuf, sizeof(tbuf), "%lldd", (long long)(remain_ms / 86400000LL));
                else if (remain_ms >= 3600000LL)
                    snprintf(tbuf, sizeof(tbuf), "%lldh", (long long)(remain_ms / 3600000LL));
                else if (remain_ms >= 60000LL)
                    snprintf(tbuf, sizeof(tbuf), "%lldm", (long long)(remain_ms / 60000LL));
                else
                    snprintf(tbuf, sizeof(tbuf), "<1m");
                char label[64];
                snprintf(label, sizeof(label), ICON_MDI_TIMER " %s", tbuf);
                ImVec2 sz = ImGui::CalcTextSize(label);
                rightX -= sz.x;
                dl->AddText(ImVec2(rightX, cardRect.Min.y + (headerH - sz.y) * 0.5f),
                    ImGui::GetColorU32(colors::Orange), label);
                rightX -= chipGap;
            }
        }

        // Security badges (weak/reused)
        rightX = DrawSecurityBadges(dl, g_shell_ptr, c.id, rightX,
            cardRect.Min.y + (headerH - ImGui::GetTextLineHeight()) * 0.5f, chipGap);

        // Chevron icon
        const char* chev = open ? ICON_MDI_CHEVRON_DOWN : ICON_MDI_CHEVRON_RIGHT;
        ImVec2 chevPos(cardRect.Min.x + padX, cardRect.Min.y + (headerH - ImGui::GetTextLineHeight()) * 0.5f);
        dl->AddText(chevPos, ImGui::GetColorU32(ImGuiCol_TextDisabled), chev);

        // Type icon / favicon before title
        float titleX = cardRect.Min.x + padX + 22.0f;
        {
            auto srv = favicon::Get(c.website);
            if (srv)
            {
                float icoSz = 24.0f;
                ImVec2 icoMin(titleX, cardRect.Min.y + (headerH - icoSz) * 0.5f);
                ImU32 favBg = IsDarkTheme() ? IM_COL32(255,255,255,18) : IM_COL32(0,0,0,12);
                dl->AddRectFilled(ImVec2(icoMin.x - 3, icoMin.y - 3), ImVec2(icoMin.x + icoSz + 3, icoMin.y + icoSz + 3), favBg, 6.0f);
                dl->AddImage((ImTextureID)srv, icoMin, ImVec2(icoMin.x + icoSz, icoMin.y + icoSz));
                titleX += icoSz + 10.0f;
            }
            else if (c.type != CredType::Password)
            {
                const char* typeIco = CredTypeIcon(c.type);
                ImVec2 icoSz = ImGui::CalcTextSize(typeIco);
                dl->AddText(ImVec2(titleX, cardRect.Min.y + (headerH - icoSz.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), typeIco);
                titleX += icoSz.x + 10.0f;
            }
        }

        // Title text (clipped if necessary)
        float titleRight = rightX + chipGap;
        if (titleRight < titleX) titleRight = titleX;

        float titleH = render::FontBold ? render::FontBold->LegacySize : ImGui::GetTextLineHeight();
        ImVec2 titlePos(titleX, cardRect.Min.y + (headerH - titleH) * 0.5f);

        dl->PushClipRect(ImVec2(titleX, cardRect.Min.y), ImVec2(titleRight, cardRect.Max.y), true);
        dl->AddText(render::FontBold, titleH, titlePos, ImGui::GetColorU32(ImGuiCol_Text), title);
        dl->PopClipRect();

        // ===========================================================
        // CARD BODY (when expanded)
        // ===========================================================
        EditBuffers& eb = g_edit[rowKey];

        const float dt_card = ImGui::GetIO().DeltaTime;
        float& storedH = g_card_body_h[rowKey];
        float& animH   = g_card_anim_h[rowKey];
        bool isClosing  = g_card_closing.count(rowKey) != 0;

        // If close was requested during measurement frame (no stored height yet), cancel it
        if (isClosing && storedH < 1.0f) {
            g_card_closing.erase(rowKey);
            isClosing = false;
        }

        bool renderBody = open || isClosing;

        // Advance animation
        if (open) {
            float targetH = (storedH > 0.0f) ? storedH : 0.0f;
            if (targetH > 0.0f) {
                float step = kCardAnimSpeed * dt_card;
                animH = ImMin(animH + step, targetH);
            }
        } else if (isClosing) {
            float step = kCardAnimSpeed * dt_card;
            animH = ImMax(animH - step, 0.0f);
            if (animH < 0.5f) {
                animH = 0.0f;
                g_card_closing.erase(rowKey);
                isClosing = false;
                renderBody = false;
            }
        }

        if (renderBody)
        {
            // Position cursor at start of body
            ImVec2 bodyStartPos = ImVec2(cardRect.Min.x, cardRect.Max.y);
            ImGui::SetCursorScreenPos(bodyStartPos);

            // Use channel splitting so shadow draws behind body content
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);  // Content channel

            // Create child window for card body with colored background
            ImU32 bodyBgColor = GetCardBodyBg();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, bodyBgColor);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, colBorder);

            // Calculate child size
            float childWidth = cardRect.Max.x - cardRect.Min.x;

            bool firstOpen = (open && storedH < 1.0f);
            bool animating = !firstOpen && (ImAbs(animH - (open ? storedH : 0.0f)) > 0.5f);

            if (firstOpen)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);

            if (animating)
                ImGui::BeginChild("card_body", ImVec2(childWidth, animH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
            else
                ImGui::BeginChild("card_body", ImVec2(childWidth, 0), ImGuiChildFlags_AutoResizeY);

            ImGui::Dummy(ImVec2(0, 12));

            // Add horizontal padding inside card
            const float bodyPadX = 30.0f;
            ImGui::Indent(bodyPadX);

            float contentW = ImGui::GetContentRegionAvail().x;

            // ===========================================================
            // TWO-COLUMN LAYOUT: Left (info) + Right (notes)
            // ===========================================================
            const float columnGap = 16.0f;
            const float itemW = 300.0f;
            const float leftColW = contentW * 0.45f;  // 45% for info
            const float rightColW = contentW - leftColW - columnGap;  // 55% for security/notes

            // ===========================================================
            // LEFT COLUMN: ITEM INFORMATION (using child to constrain width)
            // ===========================================================
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetColorU32(colors::Trans));

            ImGui::BeginChild("##left_col", ImVec2(leftColW, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
            ImGui::PushItemWidth(leftColW - 20.0f);

            const float labelW = 90.0f;
            const float rowSpacing = 6.0f;

            // Helper: Clean website URL for display
            auto CleanWebsite = [](const std::string& url) -> std::string
                {
                    std::string clean = url;
                    if (clean.rfind("https://", 0) == 0) clean = clean.substr(8);
                    else if (clean.rfind("http://", 0) == 0) clean = clean.substr(7);
                    if (clean.rfind("www.", 0) == 0) clean = clean.substr(4);
                    return clean;
                };

            if (eb.active)
            {
                // ===========================================================
                // EDIT MODE: Editable fields (type-conditional)
                // ===========================================================
                ImGui::SetNextItemWidth(itemW); InputTextString("Title##edit_title", &eb.title);

                if (c.type == CredType::Password)
                {
                    ImGui::SetNextItemWidth(itemW); InputTextString("Website##edit_website", &eb.website);
                    ImGui::SetNextItemWidth(itemW); InputTextString("User##edit_user", &eb.user);
                    ImGui::SetNextItemWidth(itemW); InputTextString("Email##edit_email", &eb.email);
                    PasswordFieldRow("##edit_pw", eb.password, eb.show_password, eb.gen_opt, itemW);
                }
                else if (c.type == CredType::CreditCard)
                {
                    ImGui::SetNextItemWidth(itemW); InputTextString("Cardholder##edit_cardholder", &eb.cardholder_name);
                    ImGui::SetNextItemWidth(itemW); InputTextFormatted("Card Number##edit_cardnum", &eb.card_number, "#### #### #### ####");
                    ImGui::SetNextItemWidth(itemW); InputTextFormatted("Expiry (MM/YY)##edit_expiry", &eb.card_expiry, "## / ##");
                    ImGui::SetNextItemWidth(itemW); InputTextString("CVV##edit_cvv", &eb.card_cvv);
                    ImGui::SetNextItemWidth(itemW); InputTextString("Brand##edit_brand", &eb.card_brand);
                    ImGui::SetNextItemWidth(itemW); InputTextString("Street Address##edit_card_addr", &eb.card_address);
                    ImGui::SetNextItemWidth(itemW); InputTextString("City##edit_card_city", &eb.card_city);
                    ImGui::SetNextItemWidth(itemW); InputTextString("Postal Code##edit_card_postal", &eb.card_postal_code);
                }
                else if (c.type == CredType::Identity)
                {
                    ImGui::SetNextItemWidth(itemW); InputTextString("Full Name##edit_fullname", &eb.full_name);
                    ImGui::SetNextItemWidth(itemW); InputTextString("ID Type##edit_idtype", &eb.id_type_buf);
                    ImGui::SetNextItemWidth(itemW); InputTextString("ID Number##edit_idnum", &eb.id_number);
                    ImGui::SetNextItemWidth(itemW); InputTextFormatted("Date of Birth##edit_dob", &eb.date_of_birth, "## / ## / ####");
                    ImGui::SetNextItemWidth(itemW); InputTextFormatted("Expiry##edit_idexpiry", &eb.expiry_date_buf, "## / ## / ####");
                    ImGui::SetNextItemWidth(itemW); InputTextString("Country##edit_country", &eb.country);
                    ImGui::SetNextItemWidth(itemW); InputTextString("Address##edit_address", &eb.address);
                    ImGui::SetNextItemWidth(itemW); InputTextFormatted("Phone##edit_phone", &eb.phone, "(###) ###-####");
                }
                // SecureNote: just title + group + notes (notes in right col)

                ImGui::SetNextItemWidth(itemW); InputTextString("Group##edit_group", &eb.group);

                ImGui::Dummy(ImVec2(0, 10));

                // Edit action buttons
                if (ImGui::Button("Save", ImVec2(90, 28)))
                {
                    out.edit_committed = true;
                    out.edited = c;
                    out.edited.title = eb.title;
                    out.edited.user = eb.user;
                    out.edited.email = eb.email;
                    out.edited.website = eb.website;
                    out.edited.group = eb.group;
                    out.edited.notes = eb.notes;
                    out.edited_password = eb.password;
                    out.edited.card_number = StripNonDigits(eb.card_number);
                    out.edited.card_expiry = StripNonDigits(eb.card_expiry);
                    out.edited.card_cvv = eb.card_cvv;
                    out.edited.card_brand = eb.card_brand;
                    out.edited.cardholder_name = eb.cardholder_name;
                    out.edited.card_address = eb.card_address;
                    out.edited.card_city = eb.card_city;
                    out.edited.card_postal_code = eb.card_postal_code;
                    out.edited.full_name = eb.full_name;
                    out.edited.id_type = eb.id_type_buf;
                    out.edited.id_number = eb.id_number;
                    out.edited.date_of_birth = StripNonDigits(eb.date_of_birth);
                    out.edited.expiry_date = StripNonDigits(eb.expiry_date_buf);
                    out.edited.country = eb.country;
                    out.edited.address = eb.address;
                    out.edited.phone = StripNonDigits(eb.phone);
                    out.interacted = true;
                    eb.active = false;
                }

                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(90, 28)))
                {
                    eb.Revert();
                    eb.active = false;
                    out.interacted = true;
                }
            }
            else
            {
                // ===========================================================
                // VIEW MODE: Inline action buttons on the right (on hover)
                // ===========================================================
                static std::set<int> visiblePasswords;

                const float iconBtnSz = 16.0f;
                const float iconColW = 24.0f;
                const float rowWidth = leftColW - 20.0f;
                const float valueColW = rowWidth - iconColW - 10.0f;
                ImGui::Spacing();

                // Type-conditional field rows
                const bool isNew = !!(c.changed_fields & FCF_IsNew);
                if (c.type == CredType::Password)
                {
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Website);
                    DrawWebsiteRow("website", c.website, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_User);
                    DrawCopyRow("username", ICON_MDI_ACCOUNT, c.user, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.user));
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Email);
                    DrawEmailRow("email", c.email, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.email));
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Password);
                    DrawPasswordRow("password", c.id, get_password_fn, c.password_history, visiblePasswords, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_TotpSecret);
                    DrawTOTPRow("totp", c.totp_secret, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing);
                }
                else if (c.type == CredType::CreditCard)
                {
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Cardholder);
                    DrawCopyRow("cardholder", ICON_MDI_ACCOUNT, c.cardholder_name, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.cardholder_name));
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardNumber);
                    DrawMaskedCopyRow("card_number", ICON_MDI_CREDIT_CARD, c.card_number, 4, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardExpiry);
                    DrawCopyRow("expiry", ICON_MDI_CALENDAR, c.card_expiry, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardCvv);
                    DrawMaskedCopyRow("cvv", ICON_MDI_LOCK, c.card_cvv, 0, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardBrand);
                    DrawInfoRow("brand", ICON_MDI_TAG, c.card_brand, iconColW, valueColW, rowSpacing);
                    if (!c.card_address.empty()) {
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardAddress);
                        DrawCopyRow("card_addr", ICON_MDI_MAP_MARKER, c.card_address, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.card_address));
                    }
                    if (!c.card_city.empty()) {
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardCity);
                        DrawInfoRow("card_city", ICON_MDI_CITY, c.card_city, iconColW, valueColW, rowSpacing);
                    }
                    if (!c.card_postal_code.empty()) {
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardPostalCode);
                        DrawInfoRow("card_postal", ICON_MDI_MAILBOX, c.card_postal_code, iconColW, valueColW, rowSpacing);
                    }
                }
                else if (c.type == CredType::Identity)
                {
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_FullName);
                    DrawCopyRow("full_name", ICON_MDI_ACCOUNT, c.full_name, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.full_name));
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_IdType);
                    DrawInfoRow("id_type", ICON_MDI_CARD_ACCOUNT_DETAILS, c.id_type, iconColW, valueColW, rowSpacing);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_IdNumber);
                    DrawCopyRow("id_number", ICON_MDI_POUND, c.id_number, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.id_number));
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_DateOfBirth);
                    DrawInfoRow("dob", ICON_MDI_CAKE_VARIANT, c.date_of_birth, iconColW, valueColW, rowSpacing);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_ExpiryDate);
                    DrawInfoRow("expiry", ICON_MDI_CALENDAR, c.expiry_date, iconColW, valueColW, rowSpacing);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Country);
                    DrawInfoRow("country", ICON_MDI_EARTH, c.country, iconColW, valueColW, rowSpacing);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Address);
                    DrawCopyRow("address", ICON_MDI_MAP_MARKER, c.address, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.address));
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Phone);
                    DrawCopyRow("phone", ICON_MDI_PHONE, c.phone, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.phone));
                }
                // SecureNote: group only (notes shown in right column)

                g_field_accent_active = isNew || !!(c.changed_fields & FCF_Group);
                DrawInfoRow("group", ICON_MDI_FOLDER, c.group, iconColW, valueColW, rowSpacing);
                g_field_accent_active = false;

                //ImGui::Dummy(ImVec2(0, 2));

                // Separator line
                {
                    ImVec2 p1 = ImGui::GetCursorScreenPos();
                    ImVec2 p2 = ImVec2(p1.x + leftColW - 20.0f, p1.y);
                    dl->AddLine(p1, p2, ImGui::GetColorU32(GetBorderGray()), 1.0f);
                    ImGui::Dummy(ImVec2(0, 10));
                }

                // Share status row
                if (g_shell_ptr)
                    DrawShareStatusRow(c, *g_shell_ptr);

                // Timestamps
                if (c.created_at_ms != 0 || c.updated_at_ms != 0)
                {
                    ImGui::PushFont(render::FontSmall);
                    if (c.created_at_ms != 0)
                        ImGui::TextDisabled("Created %s", FormatUnixMsDateLabel(c.created_at_ms).c_str());
                    if (c.updated_at_ms != 0)
                        ImGui::TextDisabled("Updated %s", FormatUnixMsDateLabel(c.updated_at_ms).c_str());
                    ImGui::PopFont();
                }

                ImGui::Dummy(ImVec2(0, 10));

                // Action buttons at bottom
                {
                    const float ibSz = 28.0f;
                    const float btnH = 26.0f;
                    const float delW = 95.0f;

                    if (!read_only)
                    {
                        if (IconSquareBtn("##ab_pin", c.is_pinned ? ICON_MDI_PIN : ICON_MDI_PIN, c.is_pinned ? "Unpin" : "Pin", ibSz, c.is_pinned, false, ImGui::GetColorU32(colors::MainColor)))
                        { out.toggle_pin_requested = true; out.interacted = true; }
                        ImGui::SameLine(0, 4);
                        if (IconSquareBtn("##ab_fav", c.is_favorite ? ICON_MDI_HEART : ICON_MDI_HEART, c.is_favorite ? "Unfavorite" : "Favorite", ibSz, c.is_favorite, false, IM_COL32(220, 60, 60, 255)))
                        { out.toggle_fav_requested = true; out.interacted = true; }
                        ImGui::SameLine(0, 4);
                    }

#ifndef PD_OFFLINE_BUILD
                    if (IconSquareBtn("##ab_share", ICON_MDI_SHARE_VARIANT, "Share", ibSz))
                    { out.anon_share_requested = true; out.interacted = true; }
#endif

                    if (!read_only)
                    {
                        ImGui::SameLine(0, 4);
                        if (IconSquareBtn("##ab_edit", ICON_MDI_PENCIL, "Edit", ibSz))
                        { out.edit_open_requested = true; out.interacted = true; }

                        // Delete — far right
                        ImGui::SameLine();
                        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - delW - 6.0f);
                        if (HoldToActionButton("##hold_delete", ICON_MDI_DELETE " Delete", 1.0f, ImVec2(delW, btnH), 4.0f))
                        { out.requested_delete = true; out.interacted = true; }
                    }
                }
                ImGui::Dummy(ImVec2(0, 4.0f));
            }

            ImGui::PopItemWidth();
            ImGui::EndChild();
            ImGui::PopStyleColor();

            // ===========================================================
            // RIGHT COLUMN: SECURITY / NOTES TABS
            // ===========================================================
            ImGui::SameLine(0, columnGap);
            const float rcPadRight = 16.0f;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetColorU32(colors::Trans));
            ImGui::BeginChild("##right_col", ImVec2(rightColW - rcPadRight, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
            ImDrawList* rcDl = ImGui::GetWindowDrawList();

            {
                bool hasSecurityRC = (c.type == CredType::Password);
                bool hasNotesRC = true; // always show notes tab (edit or view)

                static std::unordered_map<int, int> s_rc_tab;
                int& rcTab = s_rc_tab[c.id];
                if (!hasSecurityRC && rcTab == 0) rcTab = 1;

                // Deferred reprompt approval for notes tab switch
                if (g_notes_tab_reprompt_cred_id == c.id && IsRepromptApproved(RepromptAction::RevealNotes))
                {
                    rcTab = 1;
                    ClearRepromptApproval(RepromptAction::RevealNotes);
                    g_notes_tab_reprompt_cred_id = -1;
                }

                // Tab bar — icon button widget style
                {
                    const bool dark = IsDarkTheme();
                    const float tabH = 28.0f;
                    const float rounding = 8.0f;
                    const float pad_x = 6.0f;
                    const float gap = 4.0f;
                    const ImU32 mutedCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                    const ImU32 mutedCol2 = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                    const ImU32 textCol  = ImGui::GetColorU32(ImGuiCol_Text);
                    const ImU32 pillBg   = dark ? IM_COL32(40, 40, 43, 255)  : IM_COL32(235, 235, 238, 255);
                    const ImU32 pillHov  = dark ? IM_COL32(50, 50, 54, 255)  : IM_COL32(225, 225, 228, 255);
                    const ImU32 lipCol2  = dark ? IM_COL32(13, 18, 26, 92)   : IM_COL32(71, 84, 102, 56);
                    const ImU32 divCol   = dark ? IM_COL32(255,255,255, 12)  : IM_COL32(0, 0, 0, 15);
                    ImDrawList* dl = ImGui::GetWindowDrawList();

                    struct TabEntry { const char* id; const char* icon; const char* label; int idx; };
                    TabEntry tabs[2];
                    int numTabs = 0;
                    if (hasSecurityRC)
                        tabs[numTabs++] = { "##rctab_sec", ICON_MDI_SHIELD_CHECK, "Security", 0 };
                    if (hasNotesRC)
                        tabs[numTabs++] = { "##rctab_notes", ICON_MDI_NOTE_TEXT, "Notes", 1 };

                    float regionW[2] = {};
                    for (int i = 0; i < numTabs; i++)
                    {
                        bool active = (rcTab == tabs[i].idx);
                        if (active)
                        {
                            char full[128];
                            snprintf(full, sizeof(full), "%s %s", tabs[i].icon, tabs[i].label);
                            regionW[i] = ImGui::CalcTextSize(full).x + pad_x * 2.0f;
                        }
                        else
                            regionW[i] = ImGui::CalcTextSize(tabs[i].icon).x + pad_x * 2.0f;
                    }

                    float totalW = regionW[0] + (numTabs > 1 ? regionW[1] : 0.0f);
                    ImVec2 pillPos = ImGui::GetCursorScreenPos();

                    const float lipOff = 1.5f;
                    dl->AddRectFilled(
                        ImVec2(pillPos.x, pillPos.y + lipOff),
                        ImVec2(pillPos.x + totalW, pillPos.y + tabH + lipOff),
                        lipCol2, rounding);
                    dl->AddRectFilled(
                        pillPos,
                        ImVec2(pillPos.x + totalW, pillPos.y + tabH),
                        pillBg, rounding);

                    float curX = pillPos.x;
                    for (int i = 0; i < numTabs; i++)
                    {
                        bool active = (rcTab == tabs[i].idx);
                        float w = regionW[i];
                        ImVec2 rMin(curX, pillPos.y);
                        ImVec2 rMax(curX + w, pillPos.y + tabH);

                        ImGui::SetCursorScreenPos(rMin);
                        ImGui::InvisibleButton(tabs[i].id, ImVec2(w, tabH));
                        bool hov = ImGui::IsItemHovered();
                        if (ImGui::IsItemClicked())
                        {
                            if (tabs[i].idx == 1 && cfg::get_reprompt_reveal_notes())
                            {
                                g_notes_tab_reprompt_cred_id = c.id;
                                RequestReprompt(RepromptAction::RevealNotes, "");
                            }
                            else
                                rcTab = tabs[i].idx;
                        }

                        {
                            ImDrawFlags rndFlags = 0;
                            if (i == 0) rndFlags = ImDrawFlags_RoundCornersLeft;
                            else if (i == numTabs - 1) rndFlags = ImDrawFlags_RoundCornersRight;
                            if (!active)
                            {
                                ImU32 inactiveFill = dark ? IM_COL32(0, 0, 0, 40) : IM_COL32(0, 0, 0, 18);
                                dl->AddRectFilled(rMin, rMax, inactiveFill, rounding, rndFlags);
                            }
                            if (hov && !active)
                                dl->AddRectFilled(rMin, rMax, pillHov, rounding, rndFlags);
                        }

                        if (active)
                        {
                            char full[128];
                            snprintf(full, sizeof(full), "%s %s", tabs[i].icon, tabs[i].label);
                            ImVec2 sz = ImGui::CalcTextSize(full);
                            dl->AddText(ImVec2(curX + pad_x, pillPos.y + (tabH - sz.y) * 0.5f), textCol, full);
                        }
                        else
                        {
                            ImVec2 sz = ImGui::CalcTextSize(tabs[i].icon);
                            dl->AddText(ImVec2(curX + pad_x, pillPos.y + (tabH - sz.y) * 0.5f),
                                hov ? textCol : mutedCol2, tabs[i].icon);
                        }

                        curX += w;

                        // 3D divider between tabs
                        if (i < numTabs - 1)
                        {
                            ImU32 shineLine  = dark ? IM_COL32(255, 255, 255, 8) : IM_COL32(255, 255, 255, 80);
                            ImU32 shadowLine = dark ? IM_COL32(0, 0, 0, 60) : IM_COL32(0, 0, 0, 30);
                            // Flip divider based on which side is active
                            ImU32 leftLine  = active ? shineLine  : shadowLine;
                            ImU32 rightLine = active ? shadowLine : shineLine;
                            dl->AddLine(
                                ImVec2(curX, pillPos.y),
                                ImVec2(curX, pillPos.y + tabH + lipOff),
                                leftLine, 1.0f);
                            dl->AddLine(
                                ImVec2(curX + 1.0f, pillPos.y),
                                ImVec2(curX + 1.0f, pillPos.y + tabH + lipOff),
                                rightLine, 1.0f);
                            curX += 2.0f;
                        }
                    }

                    ImGui::SetCursorScreenPos(ImVec2(pillPos.x, pillPos.y + tabH + 4.0f));
                }

                // Tab content
                ImGui::Dummy(ImVec2(0, 4.0f));
                if (rcTab == 0 && hasSecurityRC && g_shell_ptr)
                {
                    DrawSecurityCard(c, *g_shell_ptr, activeVaultKey, get_password_fn, ImGui::GetContentRegionAvail().x);
                    ImGui::Dummy(ImVec2(0, 6.0f));
                }
                else if (rcTab == 1)
                {
                    const float notesH = 120.0f;
                    const float notesW = rightColW - 16.0f;

                    ImGui::PushStyleColor(ImGuiCol_FrameBg, IsDarkTheme() ? IM_COL32(255,255,255,4) : IM_COL32(0,0,0,4));
                    ImGui::PushStyleColor(ImGuiCol_Border,  IsDarkTheme() ? IM_COL32(255,255,255,20) : IM_COL32(0,0,0,20));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 10.0f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

                    if (eb.active)
                    {
                        InputTextMultilineString("##notes_edit", &eb.notes, ImVec2(notesW, notesH), 0);
                    }
                    else
                    {
                        std::string notesDisplay = c.notes.empty() ? "No notes." : c.notes;
                        InputTextMultilineString("##notes_view", &notesDisplay, ImVec2(notesW, notesH),
                            ImGuiInputTextFlags_ReadOnly);
                    }

                    ImGui::PopStyleVar(3);
                    ImGui::PopStyleColor(2);
                }
            }

            ImGui::EndChild();
            ImGui::PopStyleColor(); // ChildBg

            // Close body padding
            ImGui::Unindent(bodyPadX);
            ImGui::Dummy(ImVec2(0, 12));

            // End child window
            ImGui::EndChild();

            // Capture natural height
            float renderedH = ImGui::GetItemRectSize().y;
            if (firstOpen) {
                ImGui::PopStyleVar(); // Alpha
                storedH = renderedH;
                animH = 0.0f;
            } else if (open && !animating && renderedH > 1.0f) {
                storedH = renderedH;
                animH = renderedH;  // sync so close animation starts from correct height
            }

            // Pop child window styles
            ImGui::PopStyleColor(2);  // ChildBg, Border
            ImGui::PopStyleVar(2);    // ChildRounding, ChildBorderSize

            if (!firstOpen)
            {
                // Auto-scroll if this item was just opened (wait for animation to finish)
                auto scroll_it = g_scroll_to_open.find(rowKey);
                if (scroll_it != g_scroll_to_open.end())
                {
                    if (!animating && --scroll_it->second <= 0)
                    {
                        if (g_shell_ptr && g_shell_ptr->autoscroll_enabled)
                            ImGui::SetScrollHereY(0.45f);   // ~middle, matches Windows; was 1.0f = bottom
                        g_scroll_to_open.erase(scroll_it);
                    }
                }

                // Switch to background channel for shadow
                dl->ChannelsSetCurrent(0);

                // Body shadow — each ring starts below header shadow's corresponding extension
                ImVec2 bodyEndPos = ImGui::GetCursorScreenPos();
                ImVec2 fullCardMax = ImVec2(cardRect.Max.x, bodyEndPos.y);

                dl->AddRectFilled(ImVec2(cardRect.Min.x - 4.0f, cardRect.Max.y + 4.0f),
                    ImVec2(fullCardMax.x + 4.0f, fullCardMax.y + 4.0f), GetShadowColor(0, rowHovered), rounding, ImDrawFlags_RoundCornersBottom);
                dl->AddRectFilled(ImVec2(cardRect.Min.x - 2.0f, cardRect.Max.y + 2.0f),
                    ImVec2(fullCardMax.x + 2.0f, fullCardMax.y + 2.0f), GetShadowColor(1, rowHovered), rounding, ImDrawFlags_RoundCornersBottom);
                dl->AddRectFilled(ImVec2(cardRect.Min.x - 1.0f, cardRect.Max.y + 1.0f),
                    ImVec2(fullCardMax.x + 1.0f, fullCardMax.y + 1.0f), GetShadowColor(2, rowHovered), rounding, ImDrawFlags_RoundCornersBottom);
                dl->AddRectFilled(ImVec2(cardRect.Min.x, cardRect.Max.y), fullCardMax, GetShadowColor(3, rowHovered), rounding, ImDrawFlags_RoundCornersBottom);
            }

            // Merge channels back
            dl->ChannelsMerge();

            if (firstOpen)
            {
                // Measurement frame — reset cursor to collapsed position
                ImGui::SetCursorScreenPos(ImVec2(cardRect.Min.x, cardRect.Max.y));
                ImGui::Dummy(ImVec2(0, 8));
            }
        }
        else
        {
            ImGui::SetCursorScreenPos(ImVec2(cardRect.Min.x, cardRect.Max.y));
            ImGui::Dummy(ImVec2(0, 8));
        }

        // Hover-expand: deferred close — check full card area (header + body)
        if (g_shell_ptr && g_shell_ptr->hover_expand && g_hover_opened.count(rowKey))
        {
            ImVec2 endPos = ImGui::GetCursorScreenPos();
            ImRect fullCard(cardRect.Min, ImVec2(cardRect.Max.x, renderBody ? endPos.y : cardRect.Max.y));
            if (!fullCard.Contains(ImGui::GetMousePos()))
            {
                if (g_card_body_h.count(rowKey)) g_card_closing.insert(rowKey);
                g_open.erase(rowKey);
                g_hover_opened.erase(rowKey);
                g_notes_visible.erase(rowKey);
            }
        }

        // Final safety: don't let commits leak out in read-only
        if (read_only)
        {
            out.edit_committed = false;
            out.requested_delete = false;
        }

        ImGui::PopID();
        return out;
    }

    static std::unordered_set<uint64_t> g_revealed_fields; // tracks revealed field keys

    static bool DrawCopyFieldRow(
        const char* id,
        const char* label,
        const std::string& value,
        const std::string& display_value,
        uint64_t rowKey,
        int fieldIndex,
        float rowWidth,
        bool& interaction_consumed)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (!window || window->SkipItems)
            return false;

        const float rowH = 20.0f;
        const float padX = 10.0f;
        const float padY = 6.0f;
        const float eyeW = 20.0f;
        const float rightPad = 8.0f;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImRect rowRect(pos, ImVec2(pos.x + rowWidth, pos.y + rowH));
        const bool rowHovered = ImGui::IsMouseHoveringRect(rowRect.Min, rowRect.Max);

        DrawFieldHighlight(dl, rowRect.Min.x, rowRect.Min.y, rowWidth, rowH);

        const ImU32 bg = ImGui::GetColorU32(GetItemBg());
        const ImU32 bgHover = ImGui::GetColorU32(GetItemHoverBg());
        const ImU32 borderCol = ImGui::GetColorU32(colors::Trans);
        dl->AddRectFilled(rowRect.Min, rowRect.Max, rowHovered ? bgHover : bg, 6.0f);
        dl->AddRect(rowRect.Min, rowRect.Max, borderCol, 6.0f);

        // Eye-only right column (no separate clipboard icon — copy flashes the
        // value text green instead, matching Windows DrawCopyFieldRow).
        ImRect eyeRect(
            ImVec2(rowRect.Max.x - rightPad - eyeW, rowRect.Min.y),
            ImVec2(rowRect.Max.x - rightPad, rowRect.Max.y)
        );
        ImRect mainRect(rowRect.Min, ImVec2(eyeRect.Min.x, rowRect.Max.y));

        ImGui::PushID(id);

        // Reveal toggle
        const uint64_t revealKey = MakeCopyKey(rowKey, fieldIndex + 100);
        bool revealed = g_revealed_fields.count(revealKey) > 0;

        ImGui::SetCursorScreenPos(eyeRect.Min);
        ImGui::InvisibleButton("##eye", eyeRect.GetSize());
        const bool eye_clicked = ImGui::IsItemClicked();
        const bool eye_hovered = ImGui::IsItemHovered();
        if (eye_hovered || eye_clicked)
            interaction_consumed = true;
        if (eye_clicked)
        {
            if (revealed) g_revealed_fields.erase(revealKey);
            else          g_revealed_fields.insert(revealKey);
            revealed = !revealed;
        }

        // Main area (click to copy)
        ImGui::SetCursorScreenPos(mainRect.Min);
        ImGui::InvisibleButton("##copy_area", mainRect.GetSize());
        const bool copy_clicked = ImGui::IsItemClicked();
        const bool copy_hovered = ImGui::IsItemHovered();

        if (copy_hovered || copy_clicked)
            interaction_consumed = true;

        const uint64_t flashKey = MakeCopyKey(rowKey, fieldIndex);
        if (copy_clicked)
        {
            ImGui::SetClipboardText(value.c_str());
            g_copy_flash[flashKey] = (float)ImGui::GetTime();
            ShowToast("Copied", ToastType::Success);
        }

        // Show actual value when revealed, label/masked otherwise
        const std::string display = value.empty() ? "-" : (revealed ? value : display_value);

        ImVec2 text_sz = ImGui::CalcTextSize(display.c_str());
        float textY = rowRect.Min.y + (rowH - text_sz.y) * 0.5f;

        ImRect valueClip(
            ImVec2(rowRect.Min.x + padX, rowRect.Min.y),
            ImVec2(eyeRect.Min.x - 4.0f, rowRect.Max.y)
        );

        // Flash value text green on copy (matches Windows)
        bool copied = false;
        const float now = (float)ImGui::GetTime();
        auto it = g_copy_flash.find(flashKey);
        if (it != g_copy_flash.end())
            copied = (now - it->second) < 0.8f;

        ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
        if (copied)
        {
            float elapsed = now - it->second;
            float flash = 1.0f - elapsed / 0.8f;
            ImVec4 green(0.39f, 0.90f, 0.47f, 1.0f);
            ImVec4 normal = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            textCol = ImGui::ColorConvertFloat4ToU32(ImLerp(normal, green, flash));
        }

        ImGui::PushStyleColor(ImGuiCol_Text, textCol);
        ImGui::RenderTextClipped(
            ImVec2(rowRect.Min.x + padX, textY),
            ImVec2(valueClip.Max.x, rowRect.Max.y),
            display.c_str(), nullptr, &text_sz,
            ImVec2(0.0f, 0.0f),
            &valueClip
        );
        ImGui::PopStyleColor();

        // Eye icon + tooltip
        {
            // Match row bg (use rowHovered so it highlights with the rest of the row)
            dl->AddRectFilled(eyeRect.Min, eyeRect.Max, (rowHovered || eye_hovered) ? bgHover : bg, 0.0f);
            const char* eyeLabel = revealed ? ICON_MDI_EYE : ICON_MDI_EYE_OFF;
            ImU32 eyeCol = ImGui::GetColorU32(eye_hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            ImVec2 eyeSize = ImGui::CalcTextSize(eyeLabel);
            ImVec2 eyePos(
                eyeRect.Min.x + (eyeRect.GetWidth() - eyeSize.x) * 0.5f,
                eyeRect.Min.y + (eyeRect.GetHeight() - eyeSize.y) * 0.5f
            );
            dl->AddText(eyePos, eyeCol, eyeLabel);

            if (eye_hovered && !value.empty())
                SetTooltipPadded("%s", value.c_str());
        }

        ImGui::PopID();

        ImGui::SetCursorScreenPos(rowRect.Min);
        ImGui::Dummy(ImVec2(rowWidth, rowH + 6.0f));
        return copy_clicked;
    }

    static AccordionItemAction RenderAccordionItemDetailed(
        const AccordionItem& c,
        uint32_t activeVaultKey,
        GetPasswordFn get_password_fn,
        bool read_only,
        int displayIndex)
    {
        AccordionItemAction out{};
        out.id = c.id;

        const uint64_t rowKey = MakeRowKey(activeVaultKey, c.id);
        const bool sel = IsSelected(rowKey);
        const bool anySel = HasAnySelection(activeVaultKey);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Layout constants
        const float maxCardW = 900.0f;
        const float rawAvailW = ImGui::GetContentRegionAvail().x - 32.f;
        const float availW = ImMin(rawAvailW, maxCardW);
        const float rounding = 0.0f;
        const float headerH = 50.0f;  // Taller header for two lines
        const float padX = 12.0f;

        const float cbColW = 28.0f;  // Checkbox gutter width
        const float cbPadL = 6.0f;   // Checkbox left padding

        // Colors (theme-aware)
        const ImU32 colCard = ImGui::GetColorU32(GetCardHeaderBg());
        const ImU32 colBorder = ImGui::GetColorU32(colors::Trans);

        ImVec2 rowMin = ImGui::GetCursorScreenPos();

        // ===========================================================
        // CHECKBOX (left gutter)
        // ===========================================================
        ImRect cardRect(
            ImVec2(rowMin.x + cbColW, rowMin.y),
            ImVec2(rowMin.x + availW, rowMin.y + headerH)
        );

        ImGui::PushID((void*)(uintptr_t)rowKey);

        // Show checkbox on hover or when any selection exists
        ImVec2 mp = ImGui::GetMousePos();
        const bool anyPopup = ImGui::IsPopupOpen((const char*)NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) || (g_shell_ptr && g_shell_ptr->settings_modal_open);
        const bool rowHovered = !anyPopup && ImRect(rowMin, ImVec2(rowMin.x + availW, rowMin.y + headerH)).Contains(mp);
        const bool showCb = rowHovered || sel || anySel;

        if (showCb)
        {
            float cbY = rowMin.y + (headerH - 15.0f) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x + cbPadL, cbY));
            bool cbVal = sel;
            if (ImGui::Checkbox2("", &cbVal))
            {
                ToggleSelected(rowKey);
                out.selection_toggled = true;
                out.selected_now = IsSelected(rowKey);
                out.interacted = true;
            }
        }

        // Check if card is open (need to know for shadow height)
        const bool open = (g_open.count(rowKey) != 0);

        // ===========================================================
        // CARD SHADOW (multi-ring for subtle elevation)
        // ===========================================================
        const float shadowOffset = 2.0f;  // Slight downward offset
        const ImVec2 shadowStart = ImVec2(cardRect.Min.x, cardRect.Min.y + shadowOffset);

        // Ring 1 (outermost, most transparent)
        dl->AddRectFilled(
            ImVec2(shadowStart.x - 4.0f, shadowStart.y - 4.0f),
            ImVec2(cardRect.Max.x + 4.0f, cardRect.Max.y + 4.0f),
            GetShadowColor(0, rowHovered),
            rounding
        );

        // Ring 2
        dl->AddRectFilled(
            ImVec2(shadowStart.x - 2.0f, shadowStart.y - 2.0f),
            ImVec2(cardRect.Max.x + 2.0f, cardRect.Max.y + 2.0f),
            GetShadowColor(1, rowHovered),
            rounding
        );

        // Ring 3
        dl->AddRectFilled(
            ImVec2(shadowStart.x - 1.0f, shadowStart.y - 1.0f),
            ImVec2(cardRect.Max.x + 1.0f, cardRect.Max.y + 1.0f),
            GetShadowColor(2, rowHovered),
            rounding
        );

        // Ring 4 (innermost, most opaque)
        dl->AddRectFilled(
            shadowStart,
            cardRect.Max,
            GetShadowColor(3, rowHovered),
            rounding
        );

        // ===========================================================
        // CARD HEADER (background + click handler)
        // ===========================================================
        dl->AddRectFilled(cardRect.Min, cardRect.Max, colCard, rounding);
        if (c.is_changed)
            dl->AddRectFilled(cardRect.Min, cardRect.Max,
                (c.changed_fields & FCF_IsNew) ? kNewRowTint : kChangedRowTint, rounding);
        dl->AddRect(cardRect.Min, cardRect.Max, colBorder, rounding, 0, 1.0f);

        // Click to toggle expand
        ImGui::SetCursorScreenPos(cardRect.Min);
        ImGui::InvisibleButton("##acc_header_btn", cardRect.GetSize());

        if (ImGui::IsItemActivated())
        {
            g_hover_opened.erase(rowKey); // click-opened items persist
            if (g_open.count(rowKey)) {
                g_open.erase(rowKey);
                g_notes_visible.erase(rowKey);
                if (g_card_body_h.count(rowKey)) g_card_closing.insert(rowKey);
            }
            else {
                g_open.insert(rowKey);
                g_scroll_to_open[rowKey] = 2;  // Wait 2 frames for body to fully render
            }
            out.interacted = true;
        }

        // Hover-expand: open on header hover (close is deferred until after body render)
        if (g_shell_ptr && g_shell_ptr->hover_expand)
        {
            if (rowHovered && !g_open.count(rowKey))
            {
                g_open.insert(rowKey);
                g_hover_opened.insert(rowKey);
            }
        }

        // Right-click context menu on header (only if no other popup is open)
        bool detailedCtxAllowed = !ImGui::IsPopupOpen((const char*)NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)
                               || ImGui::IsPopupOpen("##detailed_row_ctx");
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,  8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(4, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(6, 2));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, IsDarkTheme() ? IM_COL32(26, 26, 28, 255) : IM_COL32(250, 250, 252, 255));
        ImGui::PushStyleColor(ImGuiCol_Border,  IsDarkTheme() ? IM_COL32(255,255,255,25) : IM_COL32(0,0,0,30));
        if (detailedCtxAllowed && ImGui::BeginPopupContextItem("##detailed_row_ctx"))
        {
            PopupStyleBegin();
            auto& cols = g_shell_ptr->detailed_header_cols;
            if (BarMenuItemToggle("Show Title",    &cols.title,    true, true)) cfg::set_detailed_header_columns(cols);
            if (BarMenuItemToggle("Show Username", &cols.username, true, true)) cfg::set_detailed_header_columns(cols);
            if (BarMenuItemToggle("Show Email",    &cols.email,    true, true)) cfg::set_detailed_header_columns(cols);
            if (BarMenuItemToggle("Show Date",     &cols.date,     true, true)) cfg::set_detailed_header_columns(cols);
            if (BarMenuItemToggle("Show Index",    &cols.index,    true, true)) cfg::set_detailed_header_columns(cols);
            if (BarMenuItemToggle("Show Pin/Fav",  &cols.pin_fav,  true, true)) cfg::set_detailed_header_columns(cols);
            ImGui::Separator();
            if (BarMenuItemToggle("Password Subtitle", &cols.sub_password, true, true)) cfg::set_detailed_header_columns(cols);
            if (BarMenuItemToggle("Card Subtitle",     &cols.sub_card,     true, true)) cfg::set_detailed_header_columns(cols);
            if (BarMenuItemToggle("Identity Subtitle", &cols.sub_identity, true, true)) cfg::set_detailed_header_columns(cols);
            if (BarMenuItemToggle("Note Subtitle",     &cols.sub_note,     true, true)) cfg::set_detailed_header_columns(cols);
            PopupStyleEnd();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(5);

        // ===========================================================
        // TITLE + SUBTITLE + STATUS ICONS (PIN/FAV)
        // ===========================================================
        const auto& cols = g_shell_ptr->detailed_header_cols;
        const char* title = DisplayTitle(c.title);
        const std::string lastEdited = FormatUnixMsDateOnly(c.updated_at_ms != 0 ? c.updated_at_ms : c.created_at_ms);

        const float chipGap = 6.0f;

        float rightX = cardRect.Max.x - padX;

        // Draw from right to left: Index number first (far right)
        if (cols.index && displayIndex >= 0)
        {
            char indexBuf[16];
            snprintf(indexBuf, sizeof(indexBuf), "#%d", displayIndex);
            ImVec2 tsz = ImGui::CalcTextSize(indexBuf);
            rightX -= tsz.x;
            ImVec2 indexPos(rightX, cardRect.Min.y + (headerH - ImGui::GetTextLineHeight()) * 0.5f);
            dl->AddText(indexPos, ImGui::GetColorU32(ImGuiCol_TextDisabled), indexBuf);
            rightX -= chipGap;
        }

        // Pin/Fav icons (to the left of index)
        if (cols.pin_fav)
        {
            if (c.is_favorite)
            {
                const char* fav = ICON_MDI_HEART;
                ImVec2 sz = ImGui::CalcTextSize(fav);
                rightX -= sz.x;
                dl->AddText(ImVec2(rightX, cardRect.Min.y + (headerH - sz.y) * 0.5f),
                    GetFavoriteColor(), fav);
                rightX -= chipGap;
            }
            if (c.is_pinned)
            {
                const char* pin = ICON_MDI_PIN;
                ImVec2 sz = ImGui::CalcTextSize(pin);
                rightX -= sz.x;
                dl->AddText(ImVec2(rightX, cardRect.Min.y + (headerH - sz.y) * 0.5f),
                    ImGui::GetColorU32(colors::MainColor), pin);
                rightX -= chipGap;
            }
        }
        // Timer badge
        if (c.expires_at_ms != 0)
        {
            if (c.expires_at_ms < 0) {
                const char* ico = ICON_MDI_TIMER_SAND_COMPLETE;
                ImVec2 sz = ImGui::CalcTextSize(ico);
                rightX -= sz.x;
                dl->AddText(ImVec2(rightX, cardRect.Min.y + (headerH - sz.y) * 0.5f),
                    IM_COL32(220, 50, 50, 255), ico);
                rightX -= chipGap;
            } else {
                int64_t remain_ms = c.expires_at_ms - helpers::now_unix_ms();
                if (remain_ms < 0) remain_ms = 0;
                char tbuf[32];
                if (remain_ms >= 86400000LL)
                    snprintf(tbuf, sizeof(tbuf), "%lldd", (long long)(remain_ms / 86400000LL));
                else if (remain_ms >= 3600000LL)
                    snprintf(tbuf, sizeof(tbuf), "%lldh", (long long)(remain_ms / 3600000LL));
                else if (remain_ms >= 60000LL)
                    snprintf(tbuf, sizeof(tbuf), "%lldm", (long long)(remain_ms / 60000LL));
                else
                    snprintf(tbuf, sizeof(tbuf), "<1m");
                char label[64];
                snprintf(label, sizeof(label), ICON_MDI_TIMER " %s", tbuf);
                ImVec2 sz = ImGui::CalcTextSize(label);
                rightX -= sz.x;
                dl->AddText(ImVec2(rightX, cardRect.Min.y + (headerH - sz.y) * 0.5f),
                    ImGui::GetColorU32(colors::Orange), label);
                rightX -= chipGap;
            }
        }

        // Security badges (weak/reused)
        rightX = DrawSecurityBadges(dl, g_shell_ptr, c.id, rightX,
            cardRect.Min.y + (headerH - ImGui::GetTextLineHeight()) * 0.5f, chipGap);

        // Chevron icon
        const char* chev = open ? ICON_MDI_CHEVRON_DOWN : ICON_MDI_CHEVRON_RIGHT;
        ImVec2 chevPos(cardRect.Min.x + padX, cardRect.Min.y + (headerH - ImGui::GetTextLineHeight()) * 0.5f);
        dl->AddText(chevPos, ImGui::GetColorU32(ImGuiCol_TextDisabled), chev);

        // Type icon / favicon before title
        float textX = cardRect.Min.x + padX + 22.0f;
        {
            auto srv = favicon::Get(c.website);
            if (srv)
            {
                float icoSz = 24.0f;
                ImVec2 icoMin(textX, cardRect.Min.y + (headerH - icoSz) * 0.5f);
                ImU32 favBg = IsDarkTheme() ? IM_COL32(255,255,255,18) : IM_COL32(0,0,0,12);
                dl->AddRectFilled(ImVec2(icoMin.x - 3, icoMin.y - 3), ImVec2(icoMin.x + icoSz + 3, icoMin.y + icoSz + 3), favBg, 6.0f);
                dl->AddImage((ImTextureID)srv, icoMin, ImVec2(icoMin.x + icoSz, icoMin.y + icoSz));
                textX += icoSz + 10.0f;
            }
            else if (c.type != CredType::Password)
            {
                const char* typeIco = CredTypeIcon(c.type);
                ImVec2 icoSz = ImGui::CalcTextSize(typeIco);
                dl->AddText(ImVec2(textX, cardRect.Min.y + (headerH - icoSz.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), typeIco);
                textX += icoSz.x + 10.0f;
            }
        }

        // Title (line 1) + subtitle (line 2), vertically centered
        float titleFontSize = render::FontBold ? render::FontBold->LegacySize : ImGui::GetTextLineHeight();
        float subFontSize = render::FontSmall ? render::FontSmall->LegacySize : ImGui::GetTextLineHeight();

        // Build subtitle per Credential type
        static std::string subBuf; // static to keep c_str() alive for draw
        const char* subText = nullptr;
        subBuf.clear();
        if (c.type == CredType::Password && cols.sub_password)
        {
            if (cols.email && !c.email.empty())
                subText = MaskIfPrivate(c.email) ? MaskIfPrivate(c.email) : c.email.c_str();
            else if (cols.username && !c.user.empty())
                subText = MaskIfPrivate(c.user) ? MaskIfPrivate(c.user) : c.user.c_str();
        }
        else if (c.type == CredType::CreditCard && cols.sub_card)
        {
            if (c.card_number.size() >= 4)
                subBuf = std::string("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 ") + c.card_number.substr(c.card_number.size() - 4);
            else if (!c.cardholder_name.empty())
                subBuf = MaskIfPrivate(c.cardholder_name) ? MaskIfPrivate(c.cardholder_name) : c.cardholder_name;
            if (!subBuf.empty()) subText = subBuf.c_str();
        }
        else if (c.type == CredType::Identity && cols.sub_identity)
        {
            if (!c.full_name.empty()) subText = MaskIfPrivate(c.full_name) ? MaskIfPrivate(c.full_name) : c.full_name.c_str();
        }
        else if (c.type == CredType::SecureNote && cols.sub_note)
        {
            subBuf = c.notes.substr(0, 40);
            if (!subBuf.empty()) subText = subBuf.c_str();
        }

        const float gap = 2.0f;
        float blockH = titleFontSize + (subText ? gap + subFontSize : 0.0f);
        float blockY = cardRect.Min.y + (headerH - blockH) * 0.5f;

        // Line 1: title (or username if no title)
        if (cols.title)
        {
            dl->AddText(render::FontBold, titleFontSize, ImVec2(textX, blockY), ImGui::GetColorU32(ImGuiCol_Text), title);
        }
        else if (cols.username && !c.user.empty())
        {
            dl->AddText(render::FontSmall, subFontSize,
                ImVec2(textX, blockY), ImGui::GetColorU32(ImGuiCol_TextDisabled), c.user.c_str());
        }

        // Line 2: subtitle
        if (subText)
        {
            dl->AddText(render::FontSmall, subFontSize,
                ImVec2(textX, blockY + titleFontSize + gap),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), subText);
        }

        // Date in center (date only, no time)
        if (cols.date && !lastEdited.empty())
        {
            ImVec2 size = ImGui::CalcTextSize(lastEdited.c_str());
            float centerX = cardRect.Min.x + (cardRect.GetWidth() * 0.55f) - (size.x * 0.5f);
            ImVec2 pos(centerX, cardRect.Min.y + (headerH - size.y) * 0.5f);
            dl->AddText(pos, ImGui::GetColorU32(ImGuiCol_TextDisabled), lastEdited.c_str());
        }

        // ===========================================================
        // CARD BODY (when expanded) - same as simple rows
        // ===========================================================
        const float dt_card = ImGui::GetIO().DeltaTime;
        float& storedH = g_card_body_h[rowKey];
        float& animH   = g_card_anim_h[rowKey];
        bool isClosing  = g_card_closing.count(rowKey) != 0;

        // If close was requested during measurement frame (no stored height yet), cancel it
        if (isClosing && storedH < 1.0f) {
            g_card_closing.erase(rowKey);
            isClosing = false;
        }

        bool renderBody = open || isClosing;

        // Advance animation
        if (open) {
            float targetH = (storedH > 0.0f) ? storedH : 0.0f;
            if (targetH > 0.0f) {
                float step = kCardAnimSpeed * dt_card;
                animH = ImMin(animH + step, targetH);
            }
        } else if (isClosing) {
            float step = kCardAnimSpeed * dt_card;
            animH = ImMax(animH - step, 0.0f);
            if (animH < 0.5f) {
                animH = 0.0f;
                g_card_closing.erase(rowKey);
                isClosing = false;
                renderBody = false;
            }
        }

        if (renderBody)
        {
            ImVec2 bodyStartPos = ImVec2(cardRect.Min.x, cardRect.Max.y);
            ImGui::SetCursorScreenPos(bodyStartPos);

            // Use channel splitting so shadow draws behind body content
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);  // Content channel

            ImU32 bodyBgColor = GetCardBodyBg();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, bodyBgColor);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, colBorder);

            float childWidth = cardRect.Max.x - cardRect.Min.x;

            bool firstOpen = (open && storedH < 1.0f);
            bool animating = !firstOpen && (ImAbs(animH - (open ? storedH : 0.0f)) > 0.5f);

            if (firstOpen)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);

            if (animating)
                ImGui::BeginChild("card_body", ImVec2(childWidth, animH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
            else
                ImGui::BeginChild("card_body", ImVec2(childWidth, 0), ImGuiChildFlags_AutoResizeY);

            ImGui::Dummy(ImVec2(0, 12));

            const float bodyPadX = 30.0f;
            ImGui::Indent(bodyPadX);

            float contentW = ImGui::GetContentRegionAvail().x;
            const float columnGap = 16.0f;
            const float leftColW = contentW * 0.45f;  // 45% for info
            const float rightColW = contentW - leftColW - columnGap;  // 55% for security/notes
            const float labelW = 90.0f;
            const float rowSpacing = 6.0f;

            // LEFT COLUMN: Inline action buttons on the right of each field (on hover)
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetColorU32(colors::Trans));

            ImGui::BeginChild("##left_col", ImVec2(leftColW, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
            ImGui::PushItemWidth(leftColW - 20.0f);

            static std::set<int> visiblePasswordsDetailed;

            const float iconBtnSz = 16.0f;
            const float iconColW = 30.0f;   // was 24; matches Windows, gives icon→text breathing room
            const float rowWidth = leftColW - 40.0f;
            const float valueColW = rowWidth - iconColW - 10.0f;
            ImGui::Spacing();

            // Action buttons at TOP of left column (moved from bottom to match Windows).
            // ibSz == delH so all five buttons share the same height; Delete rounding
            // raised to 6 to match the small-button chrome visually.
            {
                const float ibSz = 26.0f;
                const float btnH = ibSz;
                const float delW = 95.0f;

                if (!read_only)
                {
                    if (IconSquareBtn("##ab_pin_d", c.is_pinned ? ICON_MDI_PIN : ICON_MDI_PIN, c.is_pinned ? "Unpin" : "Pin", ibSz, c.is_pinned, false, ImGui::GetColorU32(colors::MainColor)))
                    { out.toggle_pin_requested = true; out.interacted = true; }
                    ImGui::SameLine(0, 4);
                    if (IconSquareBtn("##ab_fav_d", c.is_favorite ? ICON_MDI_HEART : ICON_MDI_HEART, c.is_favorite ? "Unfavorite" : "Favorite", ibSz, c.is_favorite, false, IM_COL32(220, 60, 60, 255)))
                    { out.toggle_fav_requested = true; out.interacted = true; }
                    ImGui::SameLine(0, 4);
                }

#ifndef PD_OFFLINE_BUILD
                if (IconSquareBtn("##ab_share_d", ICON_MDI_SHARE_VARIANT, "Share", ibSz))
                { out.anon_share_requested = true; out.interacted = true; }
#endif

                if (!read_only)
                {
                    ImGui::SameLine(0, 4);
                    if (IconSquareBtn("##ab_edit_d", ICON_MDI_PENCIL, "Edit", ibSz))
                    { out.edit_open_requested = true; out.interacted = true; }

                    // Delete — right-aligned on same line, matches Windows layout.
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - delW - 6.0f);
                    if (HoldToActionButton("##hold_delete_detailed", ICON_MDI_DELETE " Delete", 1.0f, ImVec2(delW, btnH), 6.0f))
                    { out.requested_delete = true; out.interacted = true; }
                }
                ImGui::Dummy(ImVec2(0, 6.0f));
            }

            // Type-conditional field rows, wrapped in a lifted child container (matches
            // Windows ui_views.cpp:1791–1899). Colors reuse the established macOS pattern
            // from RenderThreePaneDetailPane so the visual style is consistent. Dividers
            // between rows mirror the Windows layout.
            const bool isNew = !!(c.changed_fields & FCF_IsNew);
            {
                const float cBoxPad = 10.0f;
                bool dk = IsDarkTheme();

                LiftedChildColorSet lc;
                lc.bg          = dk ? IM_COL32(255, 255, 255, 6)  : IM_COL32(0, 0, 0, 6);
                lc.borderOuter = dk ? IM_COL32(0, 0, 0, 80)       : IM_COL32(0, 0, 0, 35);
                lc.borderInner = dk ? IM_COL32(255, 255, 255, 30) : IM_COL32(255, 255, 255, 180);
                SetLiftedChildColors(lc);

                if (BeginLiftedChild("##det_info", ImVec2(ImGui::GetContentRegionAvail().x, 0), ImGuiWindowFlags_NoScrollbar))
                {
                    ImGui::Dummy(ImVec2(0, 6.0f));
                    ImGui::Indent(cBoxPad);

                    const float innerW    = ImGui::GetContentRegionAvail().x;
                    const float innerValW = innerW - iconColW - 10.0f;

                    if (c.type == CredType::Password)
                    {
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Website);
                        DrawWebsiteRow("website", c.website, iconColW, innerValW, innerW, iconBtnSz, rowSpacing);
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_User);
                        DrawCopyRow("username", ICON_MDI_ACCOUNT, c.user, iconColW, innerValW, innerW, iconBtnSz, rowSpacing, MaskIfPrivate(c.user));
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Email);
                        DrawEmailRow("email", c.email, iconColW, innerValW, innerW, iconBtnSz, rowSpacing, MaskIfPrivate(c.email));
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Password);
                        DrawPasswordRow("password", c.id, get_password_fn, c.password_history, visiblePasswordsDetailed, iconColW, innerValW, innerW, iconBtnSz, rowSpacing);
                        if (!c.totp_secret.empty()) {
                            DrawRowDivider();
                            g_field_accent_active = isNew || !!(c.changed_fields & FCF_TotpSecret);
                            DrawTOTPRow("totp", c.totp_secret, iconColW, innerValW, innerW, iconBtnSz, rowSpacing);
                        }
                    }
                    else if (c.type == CredType::CreditCard)
                    {
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Cardholder);
                        DrawCopyRow("cardholder", ICON_MDI_ACCOUNT, c.cardholder_name, iconColW, innerValW, innerW, iconBtnSz, rowSpacing);
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardNumber);
                        DrawMaskedCopyRow("card_number", ICON_MDI_CREDIT_CARD, c.card_number, 4, iconColW, innerValW, innerW, iconBtnSz, rowSpacing);
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardExpiry);
                        DrawCopyRow("expiry", ICON_MDI_CALENDAR, c.card_expiry, iconColW, innerValW, innerW, iconBtnSz, rowSpacing);
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardCvv);
                        DrawMaskedCopyRow("cvv", ICON_MDI_LOCK, c.card_cvv, 0, iconColW, innerValW, innerW, iconBtnSz, rowSpacing);
                        if (!c.card_brand.empty()) {
                            DrawRowDivider();
                            g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardBrand);
                            DrawInfoRow("brand", ICON_MDI_TAG, c.card_brand, iconColW, innerValW, rowSpacing);
                        }
                        if (!c.card_address.empty()) {
                            DrawRowDivider();
                            g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardAddress);
                            DrawCopyRow("card_addr", ICON_MDI_MAP_MARKER, c.card_address, iconColW, innerValW, innerW, iconBtnSz, rowSpacing, MaskIfPrivate(c.card_address));
                        }
                        if (!c.card_city.empty()) {
                            DrawRowDivider();
                            g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardCity);
                            DrawInfoRow("card_city", ICON_MDI_CITY, c.card_city, iconColW, innerValW, rowSpacing);
                        }
                        if (!c.card_postal_code.empty()) {
                            DrawRowDivider();
                            g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardPostalCode);
                            DrawInfoRow("card_postal", ICON_MDI_MAILBOX, c.card_postal_code, iconColW, innerValW, rowSpacing);
                        }
                    }
                    else if (c.type == CredType::Identity)
                    {
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_FullName);
                        DrawCopyRow("full_name", ICON_MDI_ACCOUNT, c.full_name, iconColW, innerValW, innerW, iconBtnSz, rowSpacing);
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_IdType);
                        DrawInfoRow("id_type", ICON_MDI_CARD_ACCOUNT_DETAILS, c.id_type, iconColW, innerValW, rowSpacing);
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_IdNumber);
                        DrawCopyRow("id_number", ICON_MDI_POUND, c.id_number, iconColW, innerValW, innerW, iconBtnSz, rowSpacing);
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_DateOfBirth);
                        DrawInfoRow("dob", ICON_MDI_CAKE_VARIANT, c.date_of_birth, iconColW, innerValW, rowSpacing);
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_ExpiryDate);
                        DrawInfoRow("expiry", ICON_MDI_CALENDAR, c.expiry_date, iconColW, innerValW, rowSpacing);
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Country);
                        DrawInfoRow("country", ICON_MDI_EARTH, c.country, iconColW, innerValW, rowSpacing);
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Address);
                        DrawCopyRow("address", ICON_MDI_MAP_MARKER, c.address, iconColW, innerValW, innerW, iconBtnSz, rowSpacing);
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Phone);
                        DrawCopyRow("phone", ICON_MDI_PHONE, c.phone, iconColW, innerValW, innerW, iconBtnSz, rowSpacing);
                    }
                    // SecureNote: group only (notes shown in right column)

                    if (!c.group.empty()) {
                        DrawRowDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Group);
                        DrawInfoRow("group", ICON_MDI_FOLDER, c.group, iconColW, innerValW, rowSpacing);
                    }
                    g_field_accent_active = false;

                    ImGui::Dummy(ImVec2(0, 1.0f));
                    ImGui::Unindent(cBoxPad);
                }
                EndLiftedChild();
            }

            // Share status row
            if (g_shell_ptr)
                DrawShareStatusRow(c, *g_shell_ptr);

            // Timestamps moved to a full-width row below both columns (see after EndGroup).

            ImGui::PopItemWidth();
            ImGui::EndChild();
            ImGui::PopStyleColor();


            // RIGHT COLUMN: Security / Notes Tabs
            ImGui::SameLine(0, columnGap);
            const float rcPadRight = 16.0f;
            ImGui::BeginGroup();

            {
                bool hasSecurityRC = (c.type == CredType::Password);
                bool hasNotesRC = true;

                static std::unordered_map<int, int> s_rc_tab_d;
                int& rcTab = s_rc_tab_d[c.id];
                if (!hasSecurityRC && rcTab == 0) rcTab = 1;

                // Deferred reprompt approval for notes tab switch
                if (g_notes_tab_reprompt_cred_id == c.id && IsRepromptApproved(RepromptAction::RevealNotes))
                {
                    rcTab = 1;
                    ClearRepromptApproval(RepromptAction::RevealNotes);
                    g_notes_tab_reprompt_cred_id = -1;
                }

                // Tab bar — icon button widget style
                {
                    const bool dark = IsDarkTheme();
                    const float tabH = 28.0f;
                    const float rounding = 8.0f;
                    const float pad_x = 6.0f;
                    const float gap = 4.0f;
                    const ImU32 mutedCol2 = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                    const ImU32 textCol  = ImGui::GetColorU32(ImGuiCol_Text);
                    const ImU32 pillBg   = dark ? IM_COL32(40, 40, 43, 255)  : IM_COL32(235, 235, 238, 255);
                    const ImU32 pillHov  = dark ? IM_COL32(50, 50, 54, 255)  : IM_COL32(225, 225, 228, 255);
                    const ImU32 lipCol2  = dark ? IM_COL32(13, 18, 26, 92)   : IM_COL32(71, 84, 102, 56);
                    const ImU32 divCol   = dark ? IM_COL32(255,255,255, 12)  : IM_COL32(0, 0, 0, 15);
                    ImDrawList* dl = ImGui::GetWindowDrawList();

                    struct TabEntry { const char* id; const char* icon; const char* label; int idx; };
                    TabEntry tabs[2];
                    int numTabs = 0;
                    if (hasSecurityRC)
                        tabs[numTabs++] = { "##rctab_sec_d", ICON_MDI_SHIELD_CHECK, "Security", 0 };
                    if (hasNotesRC)
                        tabs[numTabs++] = { "##rctab_notes_d", ICON_MDI_NOTE_TEXT, "Notes", 1 };

                    float regionW[2] = {};
                    for (int i = 0; i < numTabs; i++)
                    {
                        bool active = (rcTab == tabs[i].idx);
                        if (active)
                        {
                            char full[128];
                            snprintf(full, sizeof(full), "%s %s", tabs[i].icon, tabs[i].label);
                            regionW[i] = ImGui::CalcTextSize(full).x + pad_x * 2.0f;
                        }
                        else
                            regionW[i] = ImGui::CalcTextSize(tabs[i].icon).x + pad_x * 2.0f;
                    }

                    float totalW = regionW[0] + (numTabs > 1 ? regionW[1] : 0.0f);
                    ImVec2 pillPos = ImGui::GetCursorScreenPos();

                    const float lipOff = 1.5f;
                    dl->AddRectFilled(
                        ImVec2(pillPos.x, pillPos.y + lipOff),
                        ImVec2(pillPos.x + totalW, pillPos.y + tabH + lipOff),
                        lipCol2, rounding);
                    dl->AddRectFilled(
                        pillPos,
                        ImVec2(pillPos.x + totalW, pillPos.y + tabH),
                        pillBg, rounding);

                    float curX = pillPos.x;
                    for (int i = 0; i < numTabs; i++)
                    {
                        bool active = (rcTab == tabs[i].idx);
                        float w = regionW[i];
                        ImVec2 rMin(curX, pillPos.y);
                        ImVec2 rMax(curX + w, pillPos.y + tabH);

                        ImGui::SetCursorScreenPos(rMin);
                        ImGui::InvisibleButton(tabs[i].id, ImVec2(w, tabH));
                        bool hov = ImGui::IsItemHovered();
                        if (ImGui::IsItemClicked())
                        {
                            if (tabs[i].idx == 1 && cfg::get_reprompt_reveal_notes())
                            {
                                g_notes_tab_reprompt_cred_id = c.id;
                                RequestReprompt(RepromptAction::RevealNotes, "");
                            }
                            else
                                rcTab = tabs[i].idx;
                        }

                        {
                            ImDrawFlags rndFlags = 0;
                            if (i == 0) rndFlags = ImDrawFlags_RoundCornersLeft;
                            else if (i == numTabs - 1) rndFlags = ImDrawFlags_RoundCornersRight;
                            if (!active)
                            {
                                ImU32 inactiveFill = dark ? IM_COL32(0, 0, 0, 40) : IM_COL32(0, 0, 0, 18);
                                dl->AddRectFilled(rMin, rMax, inactiveFill, rounding, rndFlags);
                            }
                            if (hov && !active)
                                dl->AddRectFilled(rMin, rMax, pillHov, rounding, rndFlags);
                        }

                        if (active)
                        {
                            char full[128];
                            snprintf(full, sizeof(full), "%s %s", tabs[i].icon, tabs[i].label);
                            ImVec2 sz = ImGui::CalcTextSize(full);
                            dl->AddText(ImVec2(curX + pad_x, pillPos.y + (tabH - sz.y) * 0.5f), textCol, full);
                        }
                        else
                        {
                            ImVec2 sz = ImGui::CalcTextSize(tabs[i].icon);
                            dl->AddText(ImVec2(curX + pad_x, pillPos.y + (tabH - sz.y) * 0.5f),
                                hov ? textCol : mutedCol2, tabs[i].icon);
                        }

                        curX += w;

                        // 3D divider between tabs
                        if (i < numTabs - 1)
                        {
                            ImU32 shineLine  = dark ? IM_COL32(255, 255, 255, 8) : IM_COL32(255, 255, 255, 80);
                            ImU32 shadowLine = dark ? IM_COL32(0, 0, 0, 60) : IM_COL32(0, 0, 0, 30);
                            // Flip divider based on which side is active
                            ImU32 leftLine  = active ? shineLine  : shadowLine;
                            ImU32 rightLine = active ? shadowLine : shineLine;
                            dl->AddLine(
                                ImVec2(curX, pillPos.y),
                                ImVec2(curX, pillPos.y + tabH + lipOff),
                                leftLine, 1.0f);
                            dl->AddLine(
                                ImVec2(curX + 1.0f, pillPos.y),
                                ImVec2(curX + 1.0f, pillPos.y + tabH + lipOff),
                                rightLine, 1.0f);
                            curX += 2.0f;
                        }
                    }

                    ImGui::SetCursorScreenPos(ImVec2(pillPos.x, pillPos.y + tabH + 4.0f));
                }

                // Tab content
                ImGui::Dummy(ImVec2(0, 4.0f));
                if (rcTab == 0 && hasSecurityRC && g_shell_ptr)
                {
                    DrawSecurityCard(c, *g_shell_ptr, activeVaultKey, get_password_fn, ImGui::GetContentRegionAvail().x - rcPadRight);
                    ImGui::Dummy(ImVec2(0, 6.0f));
                }
                else if (rcTab == 1)
                {
                    const float notesW = rightColW - 16.0f - rcPadRight;

                    ImGui::PushStyleColor(ImGuiCol_FrameBg, IsDarkTheme() ? IM_COL32(255,255,255,4) : IM_COL32(0,0,0,4));
                    ImGui::PushStyleColor(ImGuiCol_Border,  IsDarkTheme() ? IM_COL32(255,255,255,20) : IM_COL32(0,0,0,20));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 10.0f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

                    // Auto-fit notes height to content (matches Windows ui_views.cpp:2099-2103).
                    // Fixed 120px looked oversized for short notes like "pw ip".
                    std::string notesDisplay = c.notes.empty() ? "No notes." : c.notes;
                    ImVec2 fpad(12.0f, 10.0f);
                    float innerW = notesW - fpad.x * 2;
                    ImVec2 tSz = ImGui::CalcTextSize(notesDisplay.c_str(), nullptr, false, innerW);
                    float notesH = ImMax(tSz.y + fpad.y * 2, 40.0f);
                    InputTextMultilineString("##notes_view", &notesDisplay, ImVec2(notesW, notesH),
                        ImGuiInputTextFlags_ReadOnly);

                    ImGui::PopStyleVar(3);
                    ImGui::PopStyleColor(2);
                }
            }

            ImGui::EndGroup();

            ImGui::Unindent(bodyPadX);

            // Timestamps (full card width, below both columns) — ported from Windows.
            if (c.created_at_ms != 0 || c.updated_at_ms != 0)
            {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::Indent(bodyPadX);
                ImGui::PushFont(render::FontSmall);
                std::string ts;
                if (c.created_at_ms != 0)
                    ts = "Created " + FormatUnixMsDateLabel(c.created_at_ms);
                if (c.updated_at_ms != 0)
                {
                    if (!ts.empty()) ts += "  \xc2\xb7  ";
                    ts += "Updated " + FormatUnixMsDateLabel(c.updated_at_ms);
                }
                ImGui::TextDisabled("%s", ts.c_str());
                ImGui::PopFont();
                ImGui::Unindent(bodyPadX);
            }

            ImGui::Dummy(ImVec2(0, 12));

            ImGui::EndChild();

            // Capture natural height
            float renderedH = ImGui::GetItemRectSize().y;
            if (firstOpen) {
                ImGui::PopStyleVar(); // Alpha
                storedH = renderedH;
                animH = 0.0f;
            } else if (open && !animating && renderedH > 1.0f) {
                storedH = renderedH;
                animH = renderedH;  // sync so close animation starts from correct height
            }

            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);

            if (!firstOpen)
            {
                // Auto-scroll if this item was just opened (wait for animation to finish)
                auto scroll_it = g_scroll_to_open.find(rowKey);
                if (scroll_it != g_scroll_to_open.end())
                {
                    if (!animating && --scroll_it->second <= 0)
                    {
                        if (g_shell_ptr && g_shell_ptr->autoscroll_enabled)
                            ImGui::SetScrollHereY(0.45f);   // ~middle, matches Windows; was 1.0f = bottom
                        g_scroll_to_open.erase(scroll_it);
                    }
                }

                // Switch to background channel for shadow
                dl->ChannelsSetCurrent(0);

                // Body shadow — each ring starts below header shadow's corresponding extension
                ImVec2 bodyEndPos = ImGui::GetCursorScreenPos();
                ImVec2 fullCardMax = ImVec2(cardRect.Max.x, bodyEndPos.y);

                dl->AddRectFilled(ImVec2(cardRect.Min.x - 4.0f, cardRect.Max.y + 4.0f),
                    ImVec2(fullCardMax.x + 4.0f, fullCardMax.y + 4.0f), GetShadowColor(0, rowHovered), rounding, ImDrawFlags_RoundCornersBottom);
                dl->AddRectFilled(ImVec2(cardRect.Min.x - 2.0f, cardRect.Max.y + 2.0f),
                    ImVec2(fullCardMax.x + 2.0f, fullCardMax.y + 2.0f), GetShadowColor(1, rowHovered), rounding, ImDrawFlags_RoundCornersBottom);
                dl->AddRectFilled(ImVec2(cardRect.Min.x - 1.0f, cardRect.Max.y + 1.0f),
                    ImVec2(fullCardMax.x + 1.0f, fullCardMax.y + 1.0f), GetShadowColor(2, rowHovered), rounding, ImDrawFlags_RoundCornersBottom);
                dl->AddRectFilled(ImVec2(cardRect.Min.x, cardRect.Max.y), fullCardMax, GetShadowColor(3, rowHovered), rounding, ImDrawFlags_RoundCornersBottom);
            }

            // Merge channels back
            dl->ChannelsMerge();

            if (firstOpen)
            {
                // Measurement frame — reset cursor to collapsed position
                ImGui::SetCursorScreenPos(ImVec2(cardRect.Min.x, cardRect.Max.y));
                ImGui::Dummy(ImVec2(0, 8));
            }
        }
        else
        {
            ImGui::SetCursorScreenPos(ImVec2(cardRect.Min.x, cardRect.Max.y));
            ImGui::Dummy(ImVec2(0, 8));
        }

        // Hover-expand: deferred close — check full card area (header + body)
        if (g_shell_ptr && g_shell_ptr->hover_expand && g_hover_opened.count(rowKey))
        {
            ImVec2 endPos = ImGui::GetCursorScreenPos();
            ImRect fullCard(cardRect.Min, ImVec2(cardRect.Max.x, renderBody ? endPos.y : cardRect.Max.y));
            if (!fullCard.Contains(ImGui::GetMousePos()))
            {
                if (g_card_body_h.count(rowKey)) g_card_closing.insert(rowKey);
                g_open.erase(rowKey);
                g_hover_opened.erase(rowKey);
                g_notes_visible.erase(rowKey);
            }
        }

        if (read_only)
        {
            out.edit_open_requested = false;
            out.requested_delete = false;
        }

        ImGui::PopID();
        return out;
    }

    // ============================================================
    // Shared popup-tab content helpers
    // Used by both the tile right-click popup (RenderCredentialTile) and
    // the table right-click popup (RenderTableView). Keeps the Security
    // and Notes tabs identical across views.
    // ============================================================

    // Returns -1 if no "Also used by" sibling was clicked; otherwise the id
    // of the sibling cred to open in the editor. Callers route this to their
    // own action struct (tile uses edit_open_id_override on AccordionItemAction;
    // table writes straight to AccordionListResult.edit_open_id).
    static int RenderSecurityTabContent(const AccordionItem& c, GetPasswordFn get_password_fn)
    {
        int jumpToId = -1;
        const char* pwPtr = get_password_fn ? get_password_fn(c.id) : "";
        std::string pwStr = pwPtr ? pwPtr : "";

        // Compact strength meter at the top — gives an immediate visual readout.
        if (!pwStr.empty()) {
            float meterW = ImGui::GetContentRegionAvail().x;
            DrawStrengthMeterCompact(pwStr, meterW);
        }
        ImGui::Dummy(ImVec2(0, 4));

        // Local hint-line helper (Windows uses DrawHintLine; macOS inlines it).
        auto drawHint = [](const char* icon, const char* text, ImU32 col) {
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(icon);
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 6);
            ImGui::TextDisabled("%s", text);
        };

        // Strength hints (up to 3) + improvement count
        if (!pwStr.empty()) {
            helpers::PwStrength ps = helpers::analyze_password(pwStr);
            ImGui::PushFont(render::FontSmall);
            const ImU32 goodCol = IM_COL32(74, 197, 80, 255);
            const ImU32 warnCol = IM_COL32(220, 80, 80, 255);
            int classes = (int)ps.hasLower + (int)ps.hasUpper + (int)ps.hasDigit + (int)ps.hasSymbol;
            int hints = 0;
            if (ps.shortPwd && hints < 3)            { drawHint(ICON_MDI_CLOSE, "Too short (< 8 chars)", warnCol); hints++; }
            if (ps.allSame && hints < 3)             { drawHint(ICON_MDI_CLOSE, "All same character", warnCol); hints++; }
            if (ps.simpleSeq && hints < 3)           { drawHint(ICON_MDI_CLOSE, "Simple sequence detected", warnCol); hints++; }
            if (classes < 2 && hints < 3)            { drawHint(ICON_MDI_CLOSE, "Only one character type", warnCol); hints++; }
            if (classes >= 3 && hints < 3)           { drawHint(ICON_MDI_CHECK, "Mixed character types", goodCol); hints++; }
            if ((int)pwStr.size() >= 16 && hints < 3){ drawHint(ICON_MDI_CHECK, "Long password (16+)", goodCol); hints++; }
            // TODO: macOS PwStrength is missing isCommonPwd / isDictWord fields.
            // Add to utility's analyze_password to match Windows, then re-enable
            // those hint lines.

            int improvements = 0;
            if (ps.shortPwd)   improvements++;
            if (ps.allSame)    improvements++;
            if (ps.simpleSeq)  improvements++;
            if (!ps.hasUpper)  improvements++;
            if (!ps.hasLower)  improvements++;
            if (!ps.hasDigit)  improvements++;
            if (!ps.hasSymbol) improvements++;
            if (improvements > 0) {
                char impBuf[64];
                snprintf(impBuf, sizeof(impBuf), ICON_MDI_INFORMATION " %d improvement%s suggested",
                         improvements, improvements == 1 ? "" : "s");
                ImGui::TextDisabled("%s", impBuf);
            }
            ImGui::PopFont();
            ImGui::Spacing();
        }

        // Issue badges
        bool hasIssue = false;
        if (g_shell_ptr && g_shell_ptr->sec_weak_ids.count(c.id)) {
            ImGui::TextColored(ImVec4(174/255.f, 41/255.f, 41/255.f, 1.0f), ICON_MDI_ALERT " Weak password");
            hasIssue = true;
        }
        if (g_shell_ptr && g_shell_ptr->sec_reused_ids.count(c.id)) {
            ImGui::TextColored(ImVec4(190/255.f, 156/255.f, 63/255.f, 1.0f), ICON_MDI_REPEAT " Reused password");
            hasIssue = true;

            // "Also used by:" list — clicking jumps to the sibling cred via
            // edit_open_id_override. Only shown when we have the actual password
            // string to compare against.
            if (!pwStr.empty()) {
                ImGui::PushFont(render::FontSmall);
                ImGui::TextDisabled("Also used by:");
                const auto& creds = GetActiveVaultCreds();
                for (const auto& other : creds) {
                    if (other.id == c.id || other.is_deleted() || other.type != CredType::Password) continue;
                    if (other.password == pwStr) {
                        ImGui::PushID(other.id);
                        char label[128];
                        snprintf(label, sizeof(label), "  " ICON_MDI_ARROW_RIGHT " %s",
                                 other.title.empty() ? "(untitled)" : other.title.c_str());
                        if (ImGui::Selectable(label, false, 0, ImVec2(0, ImGui::GetTextLineHeight() + 4.0f))) {
                            jumpToId = other.id;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::PopFont();
            }
        }
        if (g_shell_ptr && g_shell_ptr->sec_exposed_ids.count(c.id)) {
            ImGui::TextColored(ImVec4(142/255.f, 68/255.f, 173/255.f, 1.0f), ICON_MDI_EARTH " Exposed in breach");
            hasIssue = true;
        }
        if (g_shell_ptr && g_shell_ptr->sec_aging_ids.count(c.id)) {
            ImGui::TextColored(ImVec4(39/255.f, 174/255.f, 157/255.f, 1.0f), ICON_MDI_CLOCK_ALERT " Password is aging");
            hasIssue = true;
        }
        if (!hasIssue) {
            ImGui::TextColored(ImVec4(76/255.f, 195/255.f, 100/255.f, 1.0f), ICON_MDI_CHECK_CIRCLE " No issues detected");
        }
        return jumpToId;
    }

    static void RenderNotesTabContent(const AccordionItem& c, uint64_t rowKey)
    {
        const bool noteLocked = cfg::get_reprompt_reveal_notes() && !g_notes_visible.count(rowKey);
        const bool dk = IsDarkTheme();

        if (!c.notes.empty() && !noteLocked) {
            static std::unordered_map<std::string, float> s_notes_copy_timers;
            float now = (float)ImGui::GetTime();
            bool justCopied = s_notes_copy_timers.count("popup_notes")
                            && (now - s_notes_copy_timers["popup_notes"]) < 1.5f;
            if (BarMenuItem(justCopied ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_CONTENT_COPY "   Copy Notes", false)) {
                ImGui::SetClipboardText(c.notes.c_str());
                s_notes_copy_timers["popup_notes"] = now;
                TriggerCopyFlash();
            }
            ImGui::Dummy(ImVec2(0, 2));
        }

        if (c.notes.empty()) {
            ImGui::TextDisabled("No notes.");
        }
        else if (noteLocked) {
            float previewW = ImGui::GetContentRegionAvail().x;
            float frameH = ImGui::GetTextLineHeight() + 16.0f;
            ImVec2 fpos = ImGui::GetCursorScreenPos();
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            pdl->AddRectFilled(fpos, ImVec2(fpos.x + previewW, fpos.y + frameH),
                dk ? IM_COL32(255,255,255,4) : IM_COL32(0,0,0,4), 6.0f);
            pdl->AddRect(fpos, ImVec2(fpos.x + previewW, fpos.y + frameH),
                dk ? IM_COL32(255,255,255,20) : IM_COL32(0,0,0,20), 6.0f);
            const char* ico = ICON_MDI_LOCK; const char* label = "Notes hidden";
            ImVec2 icoSz = ImGui::CalcTextSize(ico); ImVec2 lblSz = ImGui::CalcTextSize(label);
            float totalW = icoSz.x + 4.0f + lblSz.x;
            float cx = fpos.x + (previewW - totalW) * 0.5f;
            float cy = fpos.y + (frameH - lblSz.y) * 0.5f;
            pdl->AddText(ImVec2(cx, cy), ImGui::GetColorU32(ImGuiCol_TextDisabled), ico);
            pdl->AddText(ImVec2(cx + icoSz.x + 4.0f, cy), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
            ImGui::Dummy(ImVec2(previewW, frameH));

            ImGui::Spacing();
            if (BarMenuItem(ICON_MDI_EYE "   Reveal Notes", false)) {
                g_notes_reprompt_pending = rowKey;
                if (RequestReprompt(RepromptAction::RevealNotes, "")) {
                    g_notes_visible.insert(rowKey);
                    g_notes_reprompt_pending = 0;
                }
            }
        }
        else {
            float noteW = ImGui::GetContentRegionAvail().x;
            float maxH = ImGui::GetTextLineHeight() * 6.0f + 12.0f;
            ImVec2 fpad(8.0f, 6.0f);
            float innerW = noteW - fpad.x * 2;
            ImVec2 tSz = ImGui::CalcTextSize(c.notes.c_str(), nullptr, false, innerW);
            float notesH = ImMin(ImMax(tSz.y + fpad.y * 2, 28.0f), maxH);

            ImGui::PushStyleColor(ImGuiCol_FrameBg, dk ? IM_COL32(255,255,255,4) : IM_COL32(0,0,0,4));
            ImGui::PushStyleColor(ImGuiCol_Border,  dk ? IM_COL32(255,255,255,20) : IM_COL32(0,0,0,20));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, fpad);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            std::string notesDisplay = c.notes;
            InputTextMultilineString("##popup_notes", &notesDisplay, ImVec2(noteW, notesH),
                ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
        }
    }

    // Local popup tab-bar helper (Windows has DrawPopupTabBar; ours lives only
    // in this file). Returns nothing — mutates `currentTab` on click.
    static void DrawPopupTabs(int credId, bool hasSecurityTab, int& currentTab)
    {
        const char* labels[] = { "Details", "Notes", "Security" };
        const int numTabs = hasSecurityTab ? 3 : 2;
        const bool dk = IsDarkTheme();
        const float tabH = 24.0f;
        const float padX = 2.0f;
        const float gap = 2.0f;
        const float lineThick = 2.0f;
        ImDrawList* tdl = ImGui::GetWindowDrawList();

        float tabW[3] = {};
        float totalW = 0.0f;
        for (int i = 0; i < numTabs; i++) {
            tabW[i] = ImGui::CalcTextSize(labels[i]).x + padX * 2.0f;
            totalW += tabW[i];
        }
        totalW += gap * (numTabs - 1);

        ImVec2 origin = ImGui::GetCursorScreenPos();
        float curX = origin.x;

        ImU32 borderCol = dk ? IM_COL32(255, 255, 255, 20) : IM_COL32(0, 0, 0, 15);
        tdl->AddLine(ImVec2(origin.x, origin.y + tabH), ImVec2(origin.x + totalW, origin.y + tabH), borderCol, 1.0f);

        ImGui::PushID(credId);
        for (int i = 0; i < numTabs; i++) {
            bool active = (currentTab == i);
            float w = tabW[i];
            ImVec2 rMin(curX, origin.y);
            ImVec2 rMax(curX + w, origin.y + tabH);

            ImGui::SetCursorScreenPos(rMin);
            ImGui::PushID(i);
            ImGui::InvisibleButton("##ptab", ImVec2(w, tabH));
            bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) currentTab = i;
            ImGui::PopID();

            if (hov && !active)
                tdl->AddRectFilled(rMin, rMax, dk ? IM_COL32(255, 255, 255, 8) : IM_COL32(0, 0, 0, 6), 4.0f);

            ImVec2 tSz = ImGui::CalcTextSize(labels[i]);
            ImU32 tCol = active ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
            tdl->AddText(ImVec2(curX + padX, origin.y + (tabH - tSz.y) * 0.5f), tCol, labels[i]);

            if (active)
                tdl->AddRectFilled(
                    ImVec2(curX, origin.y + tabH - lineThick),
                    ImVec2(curX + w, origin.y + tabH),
                    ImGui::GetColorU32(colors::Orange), 1.0f);

            curX += w + gap;
        }
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + tabH + 4.0f));
    }

    static AccordionItemAction RenderCredentialTile(
        const AccordionItem& c,
        uint32_t activeVaultKey,
        GetPasswordFn get_password_fn,
        float tileW,
        float tileH)
    {
        AccordionItemAction out{};
        out.id = c.id;

        const uint64_t rowKey = MakeRowKey(activeVaultKey, c.id);
        const bool sel = IsSelected(rowKey);
        const bool anySel = HasAnySelection(activeVaultKey);
        bool interaction_consumed = false;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 rowMin = ImGui::GetCursorScreenPos();
        ImRect tileRect(rowMin, ImVec2(rowMin.x + tileW, rowMin.y + tileH));

        const float cbColW = 20.0f;
        const float pad = 10.0f;
        const float rounding = 8.0f;

        ImRect cardRect(ImVec2(tileRect.Min.x + cbColW, tileRect.Min.y), tileRect.Max);

        const bool anyPopup = ImGui::IsPopupOpen((const char*)NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) || (g_shell_ptr && g_shell_ptr->settings_modal_open);
        const bool rowHovered = !anyPopup && ImGui::IsMouseHoveringRect(tileRect.Min, tileRect.Max);

        // =========================================================
        // 1) SOFT SHADOW (bottom-heavy, thins toward top) — ported from
        //    Windows ui_views.cpp:2257-2274. Asymmetric rings give the card
        //    a "lifted off the surface" feel; the prior symmetric halo looked
        //    flat by comparison.
        // =========================================================
        {
            const float shadowOff = 3.0f;
            struct ShadowRing { float top; float side; float bot; };
            ShadowRing rings[] = {
                { 0.0f, 3.0f, 6.0f },   // outermost
                { 0.0f, 2.0f, 4.0f },
                { 0.0f, 1.0f, 2.5f },
                { 0.0f, 0.5f, 1.5f },   // innermost
            };
            for (int i = 0; i < 4; i++)
            {
                dl->AddRectFilled(
                    ImVec2(cardRect.Min.x - rings[i].side, cardRect.Min.y - rings[i].top + shadowOff),
                    ImVec2(cardRect.Max.x + rings[i].side, cardRect.Max.y + rings[i].bot + shadowOff),
                    GetShadowColor(i, rowHovered), rounding);
            }
        }

        // =========================================================
        // 2) CARD FILL (theme-aware)
        // =========================================================
        const ImU32 bg = ImGui::GetColorU32(GetTileBg());
        dl->AddRectFilled(cardRect.Min, cardRect.Max, bg, rounding);
        if (c.is_changed)
            dl->AddRectFilled(cardRect.Min, cardRect.Max,
                (c.changed_fields & FCF_IsNew) ? kNewRowTint : kChangedRowTint, rounding);

        // =========================================================
        // 3) STATUS (icons only, no border)
        // =========================================================
        const ImU32 colRed = GetFavoriteColor();   // Favorite red
        const ImU32 colBlue = ImGui::GetColorU32(colors::MainColor); // Pinned blue

        // =========================================================
        // CHECKBOX (selection)
        // =========================================================
        const bool showCb = rowHovered || sel || anySel;
        if (showCb)
        {
            float cbY = tileRect.Min.y + (tileH - 15.0f) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(tileRect.Min.x + 2.0f, cbY));
            bool cbVal = sel;
            if (ImGui::Checkbox2("", &cbVal))
            {
                ToggleSelected(rowKey);
                out.selection_toggled = true;
                out.selected_now = IsSelected(rowKey);
                out.interacted = true;
                interaction_consumed = true;
            }
        }

        // =========================================================
        // CARD CONTENT
        // =========================================================
        TileCardState& state = g_tile_state[rowKey];
        ImGui::PushClipRect(cardRect.Min, cardRect.Max, true);

        if (!state.flipped)
        {
            // Front face: title, subtitle, date (bottom left), icons (bottom right)
            const char* title = DisplayTitle(c.title);

            // Type-aware subtitle
            std::string subtitle;
            if (c.type == CredType::CreditCard) {
                if (c.card_number.size() >= 4)
                    subtitle = std::string("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 ") + c.card_number.substr(c.card_number.size() - 4);
                else if (!c.card_number.empty())
                    subtitle = c.card_number;
            } else if (c.type == CredType::Identity) {
                subtitle = MaskIfPrivate(c.full_name) ? MaskIfPrivate(c.full_name) : c.full_name;
            } else if (c.type == CredType::SecureNote) {
                subtitle = c.notes.substr(0, 60);
            } else {
                const char* m = !c.email.empty() ? MaskIfPrivate(c.email) : MaskIfPrivate(c.user);
                subtitle = m ? m : (!c.email.empty() ? c.email : c.user);
            }

            const std::string lastEdited = FormatUnixMsDateOnly(c.updated_at_ms != 0 ? c.updated_at_ms : c.created_at_ms);

            // Title (top left) with type icon for non-Password
            float titleFontSize = render::FontBold ? render::FontBold->LegacySize : ImGui::GetTextLineHeight();
            float subFontSize = render::FontSmall ? render::FontSmall->LegacySize : ImGui::GetTextLineHeight();
            float titleDrawX = cardRect.Min.x + pad;

            {
                auto srv = favicon::Get(c.website);
                if (srv)
                {
                    float icoSz = 24.0f;
                    ImVec2 icoMin(titleDrawX, cardRect.Min.y + pad + 6.0f + (titleFontSize - icoSz) * 0.5f);
                    dl->AddImage((ImTextureID)srv, icoMin, ImVec2(icoMin.x + icoSz, icoMin.y + icoSz));
                    titleDrawX += icoSz + 6.0f;
                }
                else if (c.type != CredType::Password)
                {
                    const char* typeIco = CredTypeIcon(c.type);
                    ImVec2 icoSz = ImGui::CalcTextSize(typeIco);
                    dl->AddText(ImVec2(titleDrawX, cardRect.Min.y + pad + 6.0f + (titleFontSize - icoSz.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), typeIco);
                    titleDrawX += icoSz.x + 4.0f;
                }
            }

            ImVec2 titlePos(titleDrawX, cardRect.Min.y + pad + 6.0f);
            dl->AddText(render::FontBold, titleFontSize, titlePos, ImGui::GetColorU32(ImGuiCol_Text), title);

            // Subtitle (below title)
            if (!subtitle.empty())
            {
                ImVec2 subPos(cardRect.Min.x + pad, cardRect.Min.y + pad + 6.0f + titleFontSize + 4.0f);
                dl->AddText(render::FontSmall, subFontSize, subPos, ImGui::GetColorU32(ImGuiCol_TextDisabled), subtitle.c_str());
            }

            // Bottom-right icons (pin/fav + security badges)
            {
                float iconX = cardRect.Max.x - pad;
                float iconY = cardRect.Max.y - pad - ImGui::GetTextLineHeight();

                // Heart (rightmost) - RED
                if (c.is_favorite)
                {
                    const char* fav = ICON_MDI_HEART;
                    ImVec2 sz = ImGui::CalcTextSize(fav);
                    iconX -= sz.x;
                    dl->AddText(ImVec2(iconX, iconY), colRed, fav);
                    iconX -= 6.0f;
                }

                // Thumbtack (left of heart) - BLUE
                if (c.is_pinned)
                {
                    const char* pin = ICON_MDI_PIN;
                    ImVec2 sz = ImGui::CalcTextSize(pin);
                    iconX -= sz.x;
                    dl->AddText(ImVec2(iconX, iconY), colBlue, pin);
                    iconX -= 6.0f;
                }

                // Timer badge
                if (c.expires_at_ms != 0)
                {
                    if (c.expires_at_ms < 0) {
                        const char* ico = ICON_MDI_TIMER_SAND_COMPLETE;
                        ImVec2 sz = ImGui::CalcTextSize(ico);
                        iconX -= sz.x;
                        dl->AddText(ImVec2(iconX, iconY), IM_COL32(220, 50, 50, 255), ico);
                        iconX -= 6.0f;
                    } else {
                        int64_t remain_ms = c.expires_at_ms - helpers::now_unix_ms();
                        if (remain_ms < 0) remain_ms = 0;
                        char tbuf[32];
                        if (remain_ms >= 86400000LL)
                            snprintf(tbuf, sizeof(tbuf), "%lldd", (long long)(remain_ms / 86400000LL));
                        else if (remain_ms >= 3600000LL)
                            snprintf(tbuf, sizeof(tbuf), "%lldh", (long long)(remain_ms / 3600000LL));
                        else if (remain_ms >= 60000LL)
                            snprintf(tbuf, sizeof(tbuf), "%lldm", (long long)(remain_ms / 60000LL));
                        else
                            snprintf(tbuf, sizeof(tbuf), "<1m");
                        char label[64];
                        snprintf(label, sizeof(label), ICON_MDI_TIMER " %s", tbuf);
                        ImVec2 sz = ImGui::CalcTextSize(label);
                        iconX -= sz.x;
                        dl->AddText(ImVec2(iconX, iconY), ImGui::GetColorU32(colors::Orange), label);
                        iconX -= 6.0f;
                    }
                }

                // Security badges moved outside tile (drawn after PopClipRect)
            }
        }
        else
        {
            // Back face: copy field rows inside the tile, wrapped in a lifted child
            // for visual consistency with the Detailed view. Ported from Windows
            // ui_views.cpp:2436-2665.
            const float headerH = 4.0f;
            const float bodyTop = cardRect.Min.y + headerH;
            const float bodyH = cardRect.Max.y - bodyTop - 4.0f;
            const float rowWidth_tile = cardRect.GetWidth() - pad;
            const bool popupBlocking = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

            ImGui::SetCursorScreenPos(ImVec2(cardRect.Min.x + pad * 0.5f, bodyTop));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
            if (popupBlocking) ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::BeginChild("##tile_body", ImVec2(rowWidth_tile, bodyH), false, ImGuiWindowFlags_NoScrollbar);

            float innerW = ImGui::GetContentRegionAvail().x + 2.0f;

            // Tile-local 2px divider (matches Windows DrawRowDivider(2.0f); the global
            // ui::DrawRowDivider() adds 8px spacing which is too much for tile rows).
            auto drawTileDivider = [&]() {
                ImDrawList* tdl = ImGui::GetWindowDrawList();
                ImVec2 a = ImGui::GetCursorScreenPos();
                float w = ImGui::GetContentRegionAvail().x - 6.0f;
                ImU32 divCol = IsDarkTheme() ? IM_COL32(255, 255, 255, 25) : IM_COL32(0, 0, 0, 30);
                tdl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x + w, a.y), divCol, 1.0f);
                ImGui::Dummy(ImVec2(0, 2.0f));
            };

            // Lifted child container for info rows
            {
                bool dk = IsDarkTheme();
                LiftedChildColorSet lc;
                lc.bg          = dk ? IM_COL32(255, 255, 255, 6)  : IM_COL32(0, 0, 0, 6);
                lc.borderOuter = dk ? IM_COL32(0, 0, 0, 80)       : IM_COL32(0, 0, 0, 35);
                lc.borderInner = dk ? IM_COL32(255, 255, 255, 30) : IM_COL32(255, 255, 255, 180);
                SetLiftedChildColors(lc);

                const float tileLiftPad = 6.0f;
                ImGui::Indent(tileLiftPad);
                ImGui::Spacing();
                if (BeginLiftedChild("##tile_info_lifted", ImVec2(innerW - tileLiftPad * 2.0f, 0), ImGuiWindowFlags_NoScrollbar))
                {
                    ImGui::Dummy(ImVec2(0, 2.0f));
                    float liftedW = ImGui::GetContentRegionAvail().x;
                    const bool isNew = !!(c.changed_fields & FCF_IsNew);

                    if (c.type == CredType::Password)
                    {
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_User);
                        DrawCopyFieldRow("username", "user", c.user, "user", rowKey, 0, liftedW, interaction_consumed);
                        drawTileDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Email);
                        DrawCopyFieldRow("email", "email", c.email, "email", rowKey, 1, liftedW, interaction_consumed);
                        drawTileDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Password);
                        const char* pw = get_password_fn ? get_password_fn(c.id) : "";
                        const std::string pwValue = pw ? pw : "";
                        DrawCopyFieldRow("password", "password", pwValue, "password", rowKey, 2, liftedW, interaction_consumed);
                        if (!c.totp_secret.empty()) {
                            auto totp_bytes = totp::base32_decode(c.totp_secret);
                            if (totp_bytes.size() >= 10) {
                                drawTileDivider();
                                std::string totp_code = totp::generate_code_now(totp_bytes);
                                int totp_secs = totp::seconds_remaining_now();
                                std::string totp_display = totp_code + " (" + std::to_string(totp_secs) + "s)";
                                g_field_accent_active = isNew || !!(c.changed_fields & FCF_TotpSecret);
                                DrawCopyFieldRow("totp", "totp", totp_code, totp_display, rowKey, 4, liftedW, interaction_consumed);
                            }
                        }
                    }
                    else if (c.type == CredType::CreditCard)
                    {
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Cardholder);
                        DrawCopyFieldRow("cardholder", "cardholder", c.cardholder_name, "cardholder", rowKey, 0, liftedW, interaction_consumed);
                        drawTileDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardNumber);
                        DrawCopyFieldRow("card_number", "card number", c.card_number, "card number", rowKey, 1, liftedW, interaction_consumed);
                        drawTileDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardExpiry);
                        DrawCopyFieldRow("expiry", "expiry", c.card_expiry, "expiry", rowKey, 2, liftedW, interaction_consumed);
                        drawTileDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardCvv);
                        DrawCopyFieldRow("cvv", "cvv", c.card_cvv, "cvv", rowKey, 3, liftedW, interaction_consumed);
                        // Brand intentionally not shown on tile back to match Windows.
                        if (!c.card_address.empty()) {
                            drawTileDivider();
                            g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardAddress);
                            DrawCopyFieldRow("card_addr", "address", c.card_address, "address", rowKey, 4, liftedW, interaction_consumed);
                        }
                        if (!c.card_city.empty()) {
                            drawTileDivider();
                            g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardCity);
                            DrawCopyFieldRow("card_city", "city", c.card_city, "city", rowKey, 5, liftedW, interaction_consumed);
                        }
                        if (!c.card_postal_code.empty()) {
                            drawTileDivider();
                            g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardPostalCode);
                            DrawCopyFieldRow("card_postal", "postal code", c.card_postal_code, "postal code", rowKey, 6, liftedW, interaction_consumed);
                        }
                    }
                    else if (c.type == CredType::Identity)
                    {
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_FullName);
                        DrawCopyFieldRow("full_name", "name", c.full_name, "name", rowKey, 0, liftedW, interaction_consumed);
                        drawTileDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_IdNumber);
                        DrawCopyFieldRow("id_number", "id number", c.id_number, "id number", rowKey, 1, liftedW, interaction_consumed);
                        drawTileDivider();
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Phone);
                        DrawCopyFieldRow("phone", "phone", c.phone, "phone", rowKey, 2, liftedW, interaction_consumed);
                    }
                    else if (c.type == CredType::SecureNote && !c.notes.empty())
                    {
                        // Reveal/Hide notes link gated by master-password reprompt
                        // (when cfg::get_reprompt_reveal_notes() is on). Ported from
                        // Windows ui_views.cpp:2532-2632.
                        const bool noteRepromptActive = cfg::get_reprompt_reveal_notes();
                        if (noteRepromptActive && g_notes_reprompt_pending == rowKey && IsRepromptApproved(RepromptAction::RevealNotes))
                        {
                            g_notes_visible.insert(rowKey);
                            ClearRepromptApproval(RepromptAction::RevealNotes);
                            g_notes_reprompt_pending = 0;
                        }
                        bool notesRevealed = !noteRepromptActive || g_notes_visible.count(rowKey) != 0;

                        const float notePadX = 10.0f;
                        const float notePadY = 6.0f;
                        ImGui::Indent(notePadX);
                        ImGui::Dummy(ImVec2(0, notePadY));

                        if (notesRevealed)
                        {
                            const char* linkLabel = "Hide notes";
                            const char* chevron = " " ICON_MDI_CHEVRON_UP;
                            ImVec2 textSz = ImGui::CalcTextSize(linkLabel);
                            ImVec2 chevSz = ImGui::CalcTextSize(chevron);
                            ImVec2 totalSz(textSz.x + chevSz.x, textSz.y);

                            ImVec2 pos = ImGui::GetCursorScreenPos();
                            ImGui::InvisibleButton("##tile_note_hide", totalSz);
                            bool clicked = ImGui::IsItemClicked();
                            bool hovered = ImGui::IsItemHovered();
                            if (clicked) interaction_consumed = true;

                            ImU32 col = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                            ImDrawList* dl2 = ImGui::GetWindowDrawList();
                            dl2->AddText(pos, col, linkLabel);
                            dl2->AddText(ImVec2(pos.x + textSz.x, pos.y), col, chevron);
                            if (hovered)
                            {
                                float y = pos.y + textSz.y + 1.0f;
                                dl2->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + textSz.x, y), col, 1.0f);
                            }
                            if (clicked)
                                g_notes_visible.erase(rowKey);

                            ImGui::Spacing();

                            // Read-only multiline notes (auto-fit height, like the Detailed view)
                            std::string notesDisplay = c.notes;
                            float notesW = liftedW - notePadX * 2.0f;
                            ImVec2 fpad(8.0f, 6.0f);
                            float innerNotesW = notesW - fpad.x * 2;
                            ImVec2 tSz = ImGui::CalcTextSize(notesDisplay.c_str(), nullptr, false, innerNotesW);
                            float notesH = ImMax(tSz.y + fpad.y * 2, 32.0f);

                            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
                            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, fpad);
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                            InputTextMultilineString("##tile_notes_view", &notesDisplay, ImVec2(notesW, notesH),
                                ImGuiInputTextFlags_ReadOnly);
                            if (ImGui::IsItemActive()) interaction_consumed = true;
                            ImGui::PopStyleVar(2);
                            ImGui::PopStyleColor();
                        }
                        else
                        {
                            const char* linkLabel = "Reveal notes";
                            const char* chevron = " " ICON_MDI_CHEVRON_DOWN;
                            ImVec2 textSz = ImGui::CalcTextSize(linkLabel);
                            ImVec2 chevSz = ImGui::CalcTextSize(chevron);
                            ImVec2 totalSz(textSz.x + chevSz.x, textSz.y);

                            ImVec2 pos = ImGui::GetCursorScreenPos();
                            ImGui::InvisibleButton("##tile_note_reveal", totalSz);
                            bool clicked = ImGui::IsItemClicked();
                            bool hovered = ImGui::IsItemHovered();
                            if (clicked) interaction_consumed = true;

                            ImU32 col = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                            ImDrawList* dl2 = ImGui::GetWindowDrawList();
                            dl2->AddText(pos, col, linkLabel);
                            dl2->AddText(ImVec2(pos.x + textSz.x, pos.y), col, chevron);
                            if (hovered)
                            {
                                float y = pos.y + textSz.y + 1.0f;
                                dl2->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + textSz.x, y), col, 1.0f);
                            }
                            if (clicked)
                            {
                                g_notes_reprompt_pending = rowKey;
                                if (RequestReprompt(RepromptAction::RevealNotes, ""))
                                {
                                    g_notes_visible.insert(rowKey);
                                    g_notes_reprompt_pending = 0;
                                }
                            }
                        }

                        ImGui::Dummy(ImVec2(0, notePadY));
                        ImGui::Unindent(notePadX);
                    }
                    g_field_accent_active = false;

                    ImGui::Dummy(ImVec2(0, 1.0f));
                }
                EndLiftedChild();
                ImGui::Unindent(tileLiftPad);
            }

            // Group label at bottom of back face: folder icon right-aligned, group
            // text left-aligned in FontSmall. Ported from Windows ui_views.cpp:2640-2659.
            if (!c.group.empty())
            {
                const float gPadX = 10.0f;
                ImVec2 gPos = ImGui::GetCursorScreenPos();
                float gH = ImGui::GetTextLineHeight() + 4.0f;
                ImDrawList* gdl = ImGui::GetWindowDrawList();

                ImVec2 icoSz = ImGui::CalcTextSize(ICON_MDI_FOLDER);
                gdl->AddText(
                    ImVec2(gPos.x + innerW - icoSz.x - gPadX, gPos.y + (gH - icoSz.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_MDI_FOLDER);

                ImGui::PushFont(render::FontSmall);
                ImGui::SetCursorScreenPos(ImVec2(gPos.x + gPadX, gPos.y));
                ImGui::TextDisabled("%s", c.group.c_str());
                ImGui::PopFont();
            }
            g_field_accent_active = false;

            ImGui::EndChild();
            if (popupBlocking) ImGui::PopItemFlag();
            ImGui::PopStyleColor();
        }

        ImGui::PopClipRect();

        // =========================================================
        // SECURITY BADGES (outside tile, right edge, stacked vertically)
        // =========================================================
        if (g_shell_ptr) {
            float badgeX = cardRect.Max.x + 3.0f;
            float badgeY = cardRect.Min.y;
            if (g_shell_ptr->sec_highlight_weak && g_shell_ptr->sec_weak_ids.count(c.id)) {
                dl->AddText(ImVec2(badgeX, badgeY), IM_COL32(174, 41, 41, 255), ICON_MDI_ALERT);
                badgeY += ImGui::GetTextLineHeight();
            }
            if (g_shell_ptr->sec_highlight_reused && g_shell_ptr->sec_reused_ids.count(c.id)) {
                dl->AddText(ImVec2(badgeX, badgeY), IM_COL32(190, 156, 63, 255), ICON_MDI_REPEAT);
                badgeY += ImGui::GetTextLineHeight();
            }
            if (g_shell_ptr->sec_highlight_exposed && g_shell_ptr->sec_exposed_ids.count(c.id)) {
                dl->AddText(ImVec2(badgeX, badgeY), IM_COL32(142, 68, 173, 255), ICON_MDI_EARTH);
                badgeY += ImGui::GetTextLineHeight();
            }
            if (g_shell_ptr->sec_highlight_aging && g_shell_ptr->sec_aging_ids.count(c.id)) {
                dl->AddText(ImVec2(badgeX, badgeY), IM_COL32(39, 174, 157, 255), ICON_MDI_CLOCK_ALERT);
                badgeY += ImGui::GetTextLineHeight();
            }
        }

        // =========================================================
        // INTERACTIONS
        // =========================================================
        // Right-click opens popup (skip while a modal is open so it doesn't
        // capture clicks through the modal's dim overlay) — ported from
        // Windows ui_views.cpp:2696-2823.
        char popupId[32];
        snprintf(popupId, sizeof(popupId), "##tile_popup_%d", c.id);

        static std::unordered_map<int, int> s_tile_popup_tab;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && cardRect.Contains(ImGui::GetMousePos())
            && ImGui::GetTopMostPopupModal() == nullptr)
        {
            ImGui::OpenPopup(popupId);
            s_tile_popup_tab[c.id] = 0;
            out.interacted = true;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,  8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8, 6));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, IsDarkTheme() ? IM_COL32(26, 26, 28, 255) : IM_COL32(250, 250, 252, 255));
        ImGui::PushStyleColor(ImGuiCol_Border,  IsDarkTheme() ? IM_COL32(255,255,255,25) : IM_COL32(0,0,0,30));
        if (ImGui::BeginPopup(popupId))
        {
            PopupStyleBegin();
            ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);

            const bool hasSecurityTab = (c.type == CredType::Password);
            int& tileTab = s_tile_popup_tab[c.id];
            DrawPopupTabs(c.id, hasSecurityTab, tileTab);

            // Security tab needs a minimum width to keep hint lines from wrapping.
            if (tileTab == 2) {
                float secW = ImGui::CalcTextSize(ICON_MDI_CLOSE " Contains dictionary word").x + 16.0f;
                ImGui::Dummy(ImVec2(secW, 0));
            }

            // ====== TAB 0: Details ======
            if (tileTab == 0)
            {
                ImGui::PushFont(render::FontBold);
                ImGui::TextUnformatted(DisplayTitle(c.title));
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 4));

                // Quick-action icon bar: Pin / Fav / Open Website / Open Inbox
                {
                    const float ibSz = 28.0f;
                    const float ibGap = 4.0f;
                    bool hasWebsite = !c.website.empty();
                    std::string webmail = GetWebmailUrl(c.email);

                    if (IconSquareBtn("##ctx_pin", c.is_pinned ? ICON_MDI_PIN : ICON_MDI_PIN,
                        c.is_pinned ? "Unpin" : "Pin", ibSz, c.is_pinned, false, ImGui::GetColorU32(colors::MainColor)))
                    { out.toggle_pin_requested = true; out.interacted = true; }
                    ImGui::SameLine(0, ibGap);
                    if (IconSquareBtn("##ctx_fav", c.is_favorite ? ICON_MDI_HEART : ICON_MDI_HEART,
                        c.is_favorite ? "Unfavorite" : "Favorite", ibSz, c.is_favorite, false, IM_COL32(220, 60, 60, 255)))
                    { out.toggle_fav_requested = true; out.interacted = true; }
                    ImGui::SameLine(0, ibGap);
                    if (!hasWebsite) ImGui::BeginDisabled();
                    if (IconSquareBtn("##ctx_launch", ICON_MDI_EARTH, "Open Website", ibSz))
                    {
                        if (hasWebsite)
                            system((std::string("open \"") + EnsureUrlProtocol(c.website) + "\"").c_str());
                        out.interacted = true;
                    }
                    if (!hasWebsite) ImGui::EndDisabled();
                    ImGui::SameLine(0, ibGap);
                    if (webmail.empty()) ImGui::BeginDisabled();
                    if (IconSquareBtn("##ctx_inbox", ICON_MDI_EMAIL, "Open Inbox", ibSz))
                    {
                        if (!webmail.empty())
                            system((std::string("open \"") + webmail + "\"").c_str());
                        out.interacted = true;
                    }
                    if (webmail.empty()) ImGui::EndDisabled();
                }

                ImGui::Dummy(ImVec2(0, 2));
                ImGui::Separator();

                if (BarMenuItem(ICON_MDI_PENCIL "   Edit Credential", false))
                { out.edit_open_requested = true; out.interacted = true; }
#ifndef PD_OFFLINE_BUILD
                if (BarMenuItem(ICON_MDI_SHARE_VARIANT "   Share Credential", false))
                { out.anon_share_requested = true; out.interacted = true; }
#endif
                ImGui::Separator();
                if (BarMenuItem(ICON_MDI_DELETE "   Delete Credential", false))
                { out.requested_delete = true; out.interacted = true; ImGui::CloseCurrentPopup(); }
                if (ImGui::IsItemHovered())
                {
                    ImVec2 itemMin = ImGui::GetItemRectMin(); ImVec2 itemMax = ImGui::GetItemRectMax();
                    float winX = ImGui::GetWindowPos().x; float winW = ImGui::GetWindowSize().x;
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(winX, itemMin.y), ImVec2(winX + winW, itemMax.y), IM_COL32(180, 40, 40, 50), 4.0f);
                }
            }
            // ====== TAB 1: Notes ======
            else if (tileTab == 1)
            {
                RenderNotesTabContent(c, rowKey);
            }
            // ====== TAB 2: Security (Password type only) ======
            else if (tileTab == 2 && hasSecurityTab)
            {
                int jumpToId = RenderSecurityTabContent(c, get_password_fn);
                if (jumpToId >= 0) {
                    out.edit_open_requested = true;
                    out.edit_open_id_override = jumpToId;
                    out.interacted = true;
                }
            }

            ImGui::PopItemFlag();  // AutoClosePopups
            PopupStyleEnd();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(5);

        // Hover-flip logic — skip while any popup is open so the tile behind
        // the popup doesn't flip on hover (Windows guards this the same way).
        if (g_shell_ptr && g_shell_ptr->hover_expand && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        {
            if (rowHovered && !state.flipped)
            {
                state.flipped = true;
                g_hover_flipped.insert(rowKey);
            }
            else if (!rowHovered && g_hover_flipped.count(rowKey))
            {
                state.flipped = false;
                g_hover_flipped.erase(rowKey);
                g_notes_visible.erase(rowKey);
            }
        }

        // Left-click flips the card — skip while any popup is open so clicks
        // on the popup chrome don't bleed through to the tile underneath.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && cardRect.Contains(ImGui::GetMousePos()) &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        {
            if (!interaction_consumed && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))
            {
                g_hover_flipped.erase(rowKey); // click-flipped items persist
                if (state.flipped) g_notes_visible.erase(rowKey);
                state.flipped = !state.flipped;
                out.interacted = true;
            }
        }

        ImGui::SetCursorScreenPos(tileRect.Min);
        ImGui::Dummy(ImVec2(tileW, tileH + 10.0f));
        return out;
    }

    // ============================================================
    // THREE-PANE VIEW
    // ============================================================

    static void RenderThreePaneSidebar(ShellState& s, float animW, const std::vector<AccordionItem>& allItems)
    {
        const bool collapsed = s.three_pane_sidebar_collapsed;
        const float sidebarW = animW;

        const bool dark = IsDarkTheme();
        LippedChildColorSet lippedColors;
        lippedColors.bg        = dark ? IM_COL32(33, 33, 36, 255)  : IM_COL32(235, 235, 238, 255);
        lippedColors.lip_color = dark ? IM_COL32(15, 15, 18, 255) : IM_COL32(195, 195, 200, 255);
        SetLippedChildColors(lippedColors);
        BeginLippedChild("##3p_sidebar", ImVec2(sidebarW, 0));

        const ImVec4 accentCol = colors::SecondColor;
        const ImU32 accentU32 = ImGui::GetColorU32(accentCol);
        float fullW = ImGui::GetContentRegionAvail().x; // updated after scroll child begins
        float centerW = fullW;
        float sbContentRight = 0.0f; // updated after scroll child begins

        // Use unfiltered counts from ShellState (populated in application.cpp)
        const int countAll       = s.sb_count_all;
        const int countPinned    = s.sb_count_pinned;
        const int countFav       = s.sb_count_favorites;
        const int countPasswords = s.sb_count_passwords;
        const int countCards     = s.sb_count_cards;
        const int countIdentity  = s.sb_count_identity;
        const int countNotes     = s.sb_count_notes;
        const auto& groupCounts  = s.sb_group_counts;
        // Map pill tab labels to counts
        auto GetPillCount = [&](const std::string& label) -> int {
            if (label == "Pinned") return countPinned;
            if (label == "Favorites") return countFav;
            if (label == "Recent") return -1;
            auto git = groupCounts.find(label);
            return git != groupCounts.end() ? git->second : 0;
        };

        // Suppress selectable highlight — no hover/active/selected background
        const ImVec4 transparent(0, 0, 0, 0);
        ImGui::PushStyleColor(ImGuiCol_Header,        transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,   transparent);

        // Taller selectables with vertically centered text
        const float rowH = 20.0f;
        const float sbPad = 17.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));

        // Toggle button + heading
        {
            if (!collapsed) ImGui::Indent(sbPad - 2.0f);
            const char* toggleIcon = collapsed ? ICON_MDI_CHEVRON_RIGHT : ICON_MDI_CHEVRON_LEFT;
            float btnSz = 20.0f;
            if (collapsed)
            {
                float cx = (centerW - btnSz) * 0.5f;
                if (cx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
            }
            float btnY = ImGui::GetCursorPosY();
            if (IconButtonDoubleLip("##sb_toggle", toggleIcon, btnSz))
            {
                s.three_pane_sidebar_collapsed = !collapsed;
                cfg::set_three_pane_sidebar_collapsed(s.three_pane_sidebar_collapsed);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("##sb_auto_popup");
            if (!collapsed)
            {
                ImGui::SameLine(0, 6);
                float textOffY = (btnSz - ImGui::GetTextLineHeight()) * 0.5f;
                ImGui::SetCursorPosY(btnY + textOffY);
                ImGui::TextDisabled("FILTER");
            }

            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,  8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, IsDarkTheme() ? IM_COL32(26, 26, 28, 255) : IM_COL32(250, 250, 252, 255));
            ImGui::PushStyleColor(ImGuiCol_Border,  IsDarkTheme() ? IM_COL32(255,255,255,25) : IM_COL32(0,0,0,30));
            if (ImGui::BeginPopup("##sb_auto_popup"))
            {
                PopupStyleBegin();
                ImGui::TextDisabled("Auto-collapse");
                ImGui::Separator();
                bool sbAuto = s.three_pane_sidebar_auto_collapse;
                if (ImGui::Checkbox3("Sidebar", &sbAuto))
                {
                    s.three_pane_sidebar_auto_collapse = sbAuto;
                    cfg::set_three_pane_sidebar_auto_collapse(sbAuto);
                    if (sbAuto)
                    {
                        s.three_pane_sidebar_collapsed = true;
                        cfg::set_three_pane_sidebar_collapsed(true);
                    }
                    else
                    {
                        g_3p_sidebar_hover_expanded = false;
                        g_3p_sidebar_leave_timer = 0.0f;
                    }
                }
                PopupStyleEnd();
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);

            if (!collapsed) ImGui::Unindent(sbPad - 2.0f);
            ImGui::Spacing();
        }

        // Scrollable content area (header stays fixed above)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 4.0f));
        ImGui::BeginChild("##3p_sb_scroll", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        fullW = ImGui::GetContentRegionAvail().x; // recompute inside scroll child
        centerW = collapsed ? fullW : fullW;
        sbContentRight = ImGui::GetWindowPos().x + fullW; // right edge of content area

        if (collapsed)
        {
            // ---- COLLAPSED MODE: icon-only strip ----
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Filters (icon-only, centered) — sync with pill tab labels
            const char* ico0 = nullptr; const char* ico0a = nullptr;
            const char* ico1 = nullptr; const char* ico1a = nullptr;
            GetPillTabIcons(s.pill_tab_0, ico0, ico0a);
            GetPillTabIcons(s.pill_tab_1, ico1, ico1a);

            struct FilterEntry { const char* icon; const char* icon_active; const char* tooltip; int sort_val; };
            FilterEntry filters[] = {
                { ICON_MDI_FORMAT_LIST_BULLETED, ICON_MDI_FORMAT_LIST_BULLETED, "All Items",  2 },
                { ico0, ico0a, s.pill_tab_0.c_str(),  0 },
                { ico1, ico1a, s.pill_tab_1.c_str(),  1 },
            };
            for (auto& f : filters)
            {
                bool active = (s.sort_mode == f.sort_val);
                const char* ico = active ? f.icon_active : f.icon;
                ImVec2 icoSz = ImGui::CalcTextSize(ico);
                float cx = (centerW - icoSz.x) * 0.5f;
                if (cx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
                ImVec2 curPos = ImGui::GetCursorScreenPos();
                bool hovered = ImGui::IsMouseHoveringRect(curPos, ImVec2(curPos.x + fullW, curPos.y + rowH));

                // Active indicator + rounded highlight (full width)
                if (active)
                {
                    ImVec2 rowMin = ImVec2(ImGui::GetWindowPos().x, curPos.y);
                    ImVec2 rowMax = ImVec2(rowMin.x + fullW, curPos.y + rowH);
                    bool dk = IsDarkTheme();
                    dl->AddLine(rowMin, ImVec2(rowMax.x, rowMin.y),
                        dk ? IM_COL32(0, 0, 0, 30) : IM_COL32(0, 0, 0, 18), 1.0f);
                    dl->AddLine(ImVec2(rowMin.x, rowMax.y), rowMax,
                        dk ? IM_COL32(0, 0, 0, 50) : IM_COL32(0, 0, 0, 30), 1.0f);
                    ImU32 hlCol = dk ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 140);
                    dl->AddRectFilled(rowMin, rowMax, hlCol, 0.0f);
                }

                {
                    ImVec4 colInactive = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                    ImGui::PushStyleColor(ImGuiCol_Text, (active || hovered) ? accentCol : colInactive);
                    if (ImGui::Selectable(ico, active, 0, ImVec2(fullW, rowH)))
                        s.sort_mode = f.sort_val;
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered())
                    SetTooltipPadded("%s", f.tooltip);
            }

            ImGui::Spacing();

            // Types section collapse caret (centered)
            {
                const char* chevron = s.sidebar_types_collapsed ? ICON_MDI_CHEVRON_RIGHT : ICON_MDI_CHEVRON_DOWN;
                ImVec2 icoSz = ImGui::CalcTextSize(chevron);
                float cx = (centerW - icoSz.x) * 0.5f;
                if (cx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                if (ImGui::Selectable(chevron, false, 0, ImVec2(fullW, rowH)))
                    s.sidebar_types_collapsed = !s.sidebar_types_collapsed;
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) SetTooltipPadded("Types");
            }

            // Clear types X (always visible when a type filter is active)
            {
                bool anyTypeActive = false;
                for (const auto& key : s.selected_groups)
                    if (!key.empty() && key[0] == '@') { anyTypeActive = true; break; }
                if (anyTypeActive)
                {
                    const char* ico = ICON_MDI_CLOSE;
                    ImVec2 icoSz = ImGui::CalcTextSize(ico);
                    float cx = (centerW - icoSz.x) * 0.5f;
                    if (cx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::Selectable(ico, false, 0, ImVec2(fullW, rowH)))
                    {
                        std::erase_if(s.selected_groups, [](const std::string& k) {
                            return !k.empty() && k[0] == '@';
                        });
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) SetTooltipPadded("Clear Types");
                }
            }

            if (!s.sidebar_types_collapsed)
            {

            // Types (icon-only, centered) — outline when inactive, filled when active
            struct TypeEntry { const char* icon; const char* icon_active; const char* tooltip; const char* group_key; };
            TypeEntry types[] = {
                { ICON_MDI_KEY,         ICON_MDI_KEY,         "Passwords", "@Passwords" },
                { ICON_MDI_CREDIT_CARD, ICON_MDI_CREDIT_CARD, "Cards",     "@Cards" },
                { ICON_MDI_CARD_ACCOUNT_DETAILS, ICON_MDI_CARD_ACCOUNT_DETAILS, "Identity",  "@Identity" },
                { ICON_MDI_NOTE_TEXT,    ICON_MDI_NOTE_TEXT,   "Notes",     "@Notes" },
            };
            for (auto& t : types)
            {
                bool active = s.selected_groups.count(t.group_key) > 0;
                const char* ico = active ? t.icon_active : t.icon;
                ImVec2 icoSz = ImGui::CalcTextSize(ico);
                float cx = (centerW - icoSz.x) * 0.5f;
                if (cx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
                ImVec2 curPos = ImGui::GetCursorScreenPos();
                bool hovered = ImGui::IsMouseHoveringRect(curPos, ImVec2(curPos.x + fullW, curPos.y + rowH));

                if (active)
                {
                    ImVec2 rowMin = ImVec2(ImGui::GetWindowPos().x, curPos.y);
                    ImVec2 rowMax = ImVec2(rowMin.x + fullW, curPos.y + rowH);
                    bool dk = IsDarkTheme();
                    dl->AddLine(rowMin, ImVec2(rowMax.x, rowMin.y),
                        dk ? IM_COL32(0, 0, 0, 30) : IM_COL32(0, 0, 0, 18), 1.0f);
                    dl->AddLine(ImVec2(rowMin.x, rowMax.y), rowMax,
                        dk ? IM_COL32(0, 0, 0, 50) : IM_COL32(0, 0, 0, 30), 1.0f);
                    ImU32 hlCol = dk ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 140);
                    dl->AddRectFilled(rowMin, rowMax, hlCol, 0.0f);
                }

                {
                    ImVec4 colInactive = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                    ImGui::PushStyleColor(ImGuiCol_Text, (active || hovered) ? accentCol : colInactive);
                    if (ImGui::Selectable(ico, active, 0, ImVec2(fullW, rowH)))
                    {
                        if (active) s.selected_groups.erase(t.group_key);
                        else        s.selected_groups.insert(t.group_key);
                    }
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered()) SetTooltipPadded("%s", t.tooltip);
            }

            } // end if (!s.sidebar_types_collapsed)

            ImGui::Spacing();

            // Groups section collapse caret (centered)
            {
                const char* chevron = s.sidebar_groups_collapsed ? ICON_MDI_CHEVRON_RIGHT : ICON_MDI_CHEVRON_DOWN;
                ImVec2 icoSz = ImGui::CalcTextSize(chevron);
                float cx = (centerW - icoSz.x) * 0.5f;
                if (cx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::PushID("##grp_caret");
                if (ImGui::Selectable(chevron, false, 0, ImVec2(fullW, rowH)))
                    s.sidebar_groups_collapsed = !s.sidebar_groups_collapsed;
                ImGui::PopID();
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) SetTooltipPadded("Groups");
            }

            // Clear groups X (always visible when a group filter is active)
            {
                bool anyGroupActive = false;
                for (const auto& key : s.selected_groups)
                    if (key.empty() || key[0] != '@') { anyGroupActive = true; break; }
                if (anyGroupActive)
                {
                    ImGui::PushID("##clear_grp");
                    const char* ico = ICON_MDI_CLOSE;
                    ImVec2 icoSz = ImGui::CalcTextSize(ico);
                    float cx = (centerW - icoSz.x) * 0.5f;
                    if (cx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::Selectable(ico, false, 0, ImVec2(fullW, rowH)))
                    {
                        std::erase_if(s.selected_groups, [](const std::string& k) {
                            return k.empty() || k[0] != '@';
                        });
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) SetTooltipPadded("Clear Groups");
                    ImGui::PopID();
                }
            }

            if (!s.sidebar_groups_collapsed)
            {
                // Groups (centered folder icons — outline inactive, filled active)
                {
                    for (const auto& g : s.groups)
                    {
                        if (g == "Filter" || g == "All") continue;
                        if (!g.empty() && g[0] == '@') continue;
                        if (g == "---") continue;

                        bool active = s.selected_groups.count(g) > 0;
                        const char* ico = active ? ICON_MDI_FOLDER : ICON_MDI_FOLDER;

                        ImGui::PushID(g.c_str());
                        ImVec2 icoSz = ImGui::CalcTextSize(ico);
                        float cx = (centerW - icoSz.x) * 0.5f;
                        if (cx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
                        ImVec2 curPos = ImGui::GetCursorScreenPos();
                        bool hovered = ImGui::IsMouseHoveringRect(curPos, ImVec2(curPos.x + fullW, curPos.y + rowH));

                        if (active)
                        {
                            ImVec2 rowMin = ImVec2(ImGui::GetWindowPos().x, curPos.y);
                            ImVec2 rowMax = ImVec2(rowMin.x + fullW, curPos.y + rowH);
                            bool dk = IsDarkTheme();
                            dl->AddLine(rowMin, ImVec2(rowMax.x, rowMin.y),
                                dk ? IM_COL32(0, 0, 0, 30) : IM_COL32(0, 0, 0, 18), 1.0f);
                            dl->AddLine(ImVec2(rowMin.x, rowMax.y), rowMax,
                                dk ? IM_COL32(0, 0, 0, 50) : IM_COL32(0, 0, 0, 30), 1.0f);
                            ImU32 hlCol = dk ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 140);
                            dl->AddRectFilled(rowMin, rowMax, hlCol, 0.0f);
                        }

                        // Default icon color = gray (TextDisabled), accent when active/hovered.
                        ImVec4 colInactive = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                        ImGui::PushStyleColor(ImGuiCol_Text, (active || hovered) ? accentCol : colInactive);
                        if (ImGui::Selectable(ico, active, 0, ImVec2(fullW, rowH)))
                        {
                            if (active) s.selected_groups.erase(g);
                            else        s.selected_groups.insert(g);
                        }
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) SetTooltipPadded("%s", g.c_str());

                        ImGui::PopID();
                    }
                }
            } // end if (!s.sidebar_groups_collapsed)

            // --- TAGS section (collapsed sidebar, inline icon strip) ---
            // Replaces the prior button+popup with Windows-style inline rendering.
            if (!s.all_tags.empty())
            {
                ImGui::Spacing();

                // Chevron header (centered, toggles sidebar_tags_collapsed)
                {
                    const char* chevron = s.sidebar_tags_collapsed ? ICON_MDI_CHEVRON_RIGHT : ICON_MDI_CHEVRON_DOWN;
                    ImVec2 icoSz = ImGui::CalcTextSize(chevron);
                    float cx = (centerW - icoSz.x) * 0.5f;
                    if (cx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::PushID("##tag_caret_c");
                    if (ImGui::Selectable(chevron, false, 0, ImVec2(fullW, rowH)))
                        s.sidebar_tags_collapsed = !s.sidebar_tags_collapsed;
                    ImGui::PopID();
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) SetTooltipPadded("TAGS");
                }

                if (!s.sidebar_tags_collapsed)
                {
                    bool anyTagActive = !s.selected_tags.empty();
                    if (anyTagActive)
                    {
                        const char* xIco = ICON_MDI_CLOSE;
                        ImVec2 icoSz = ImGui::CalcTextSize(xIco);
                        float cx = (centerW - icoSz.x) * 0.5f;
                        if (cx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        ImGui::PushID("##clear_tags_c");
                        if (ImGui::Selectable(xIco, false, 0, ImVec2(fullW, rowH)))
                            s.selected_tags.clear();
                        ImGui::PopID();
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) SetTooltipPadded("Clear Tags");
                    }

                    for (const auto& tag : s.all_tags)
                    {
                        bool active = s.selected_tags.count(tag) > 0;
                        const char* ico = ICON_MDI_TAG;
                        ImVec2 curPos = ImGui::GetCursorScreenPos();

                        // Active row highlight (matches Groups section style)
                        if (active)
                        {
                            ImDrawList* dl_t = ImGui::GetWindowDrawList();
                            float hlLeft = ImGui::GetWindowPos().x;
                            dl_t->AddLine(
                                ImVec2(hlLeft, curPos.y),
                                ImVec2(sbContentRight, curPos.y),
                                dark ? IM_COL32(0, 0, 0, 30) : IM_COL32(0, 0, 0, 18), 1.0f);
                            dl_t->AddLine(
                                ImVec2(hlLeft, curPos.y + rowH),
                                ImVec2(sbContentRight, curPos.y + rowH),
                                dark ? IM_COL32(0, 0, 0, 50) : IM_COL32(0, 0, 0, 30), 1.0f);
                            ImU32 hlCol = dark ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 140);
                            dl_t->AddRectFilled(
                                ImVec2(hlLeft, curPos.y),
                                ImVec2(sbContentRight, curPos.y + rowH),
                                hlCol, 0.0f);
                        }

                        ImVec2 selPos = ImGui::GetCursorScreenPos();
                        ImGui::PushID(tag.c_str());
                        ImGui::InvisibleButton("##ctg", ImVec2(fullW, rowH));
                        if (ImGui::IsItemClicked()) {
                            if (active) s.selected_tags.erase(tag);
                            else        s.selected_tags.insert(tag);
                        }
                        bool itemHov = ImGui::IsItemHovered();
                        ImGui::PopID();

                        ImU32 icoCol = active ? accentU32
                                              : (itemHov ? accentU32
                                                         : ImGui::GetColorU32(ImGuiCol_TextDisabled));
                        float icoY = selPos.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
                        ImDrawList* dl_t2 = ImGui::GetWindowDrawList();
                        dl_t2->AddText(
                            ImVec2(selPos.x + (centerW - ImGui::CalcTextSize(ico).x) * 0.5f, icoY),
                            icoCol, ico);
                        if (itemHov) SetTooltipPadded("%s", tag.c_str());
                    }
                }
            }
        }
        else
        {
            // ---- EXPANDED MODE (existing code) ----
            ImGui::Indent(sbPad);

            // --- High-level filters (sync with pill tab labels) ---
            const char* eIco0 = nullptr; const char* eIco0a = nullptr;
            const char* eIco1 = nullptr; const char* eIco1a = nullptr;
            GetPillTabIcons(s.pill_tab_0, eIco0, eIco0a);
            GetPillTabIcons(s.pill_tab_1, eIco1, eIco1a);

            char label0[128], label0a[128], label1[128], label1a[128];
            snprintf(label0,  sizeof(label0),  "%s  %s", eIco0,  s.pill_tab_0.c_str());
            snprintf(label0a, sizeof(label0a), "%s  %s", eIco0a, s.pill_tab_0.c_str());
            snprintf(label1,  sizeof(label1),  "%s  %s", eIco1,  s.pill_tab_1.c_str());
            snprintf(label1a, sizeof(label1a), "%s  %s", eIco1a, s.pill_tab_1.c_str());

            struct FilterEntry { const char* label; const char* label_active; int sort_val; };
            FilterEntry filters[] = {
                { ICON_MDI_FORMAT_LIST_BULLETED "  All Items", ICON_MDI_FORMAT_LIST_BULLETED "  All Items",  2 },
                { label0, label0a, 0 },
                { label1, label1a, 1 },
            };
            int filterCounts[] = { countAll, GetPillCount(s.pill_tab_0), GetPillCount(s.pill_tab_1) };
            for (int fi = 0; fi < 3; fi++)
            {
                auto& f = filters[fi];
                bool active = (s.sort_mode == f.sort_val);
                const char* lbl = active ? f.label_active : f.label;
                ImVec2 curPos = ImGui::GetCursorScreenPos();
                bool hovered = ImGui::IsMouseHoveringRect(curPos, ImVec2(curPos.x + fullW, curPos.y + rowH));

                // Active indicator + rounded highlight (full width)
                if (active)
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    float hlLeft = ImGui::GetWindowPos().x;
                    // Top shadow line (lighter)
                    dl->AddLine(
                        ImVec2(hlLeft, curPos.y),
                        ImVec2(sbContentRight, curPos.y),
                        dark ? IM_COL32(0, 0, 0, 30) : IM_COL32(0, 0, 0, 18), 1.0f);
                    // Bottom shadow line (darker)
                    dl->AddLine(
                        ImVec2(hlLeft, curPos.y + rowH),
                        ImVec2(sbContentRight, curPos.y + rowH),
                        dark ? IM_COL32(0, 0, 0, 50) : IM_COL32(0, 0, 0, 30), 1.0f);
                    // Fill
                    ImU32 hlCol = dark ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 140);
                    dl->AddRectFilled(
                        ImVec2(hlLeft, curPos.y),
                        ImVec2(sbContentRight, curPos.y + rowH),
                        hlCol, 0.0f);
                    // Rounded accent pill on left edge
                }

                if (active)
                {
                    // Split: draw icon in accent, text in default
                    const char* sp = strchr(lbl, ' ');
                    ImVec2 selPos = ImGui::GetCursorScreenPos();
                    ImGui::PushID(fi);
                    ImGui::InvisibleButton("##fsel", ImVec2(fullW, rowH));
                    if (ImGui::IsItemClicked()) s.sort_mode = f.sort_val;
                    ImGui::PopID();
                    ImDrawList* dlIco = ImGui::GetWindowDrawList();
                    float textY = selPos.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
                    // Icon in accent
                    char icoBuf[64];
                    int icoLen = sp ? (int)(sp - lbl) : (int)strlen(lbl);
                    if (icoLen >= (int)sizeof(icoBuf)) icoLen = (int)sizeof(icoBuf) - 1;
                    memcpy(icoBuf, lbl, icoLen); icoBuf[icoLen] = 0;
                    dlIco->AddText(ImVec2(selPos.x, textY), accentU32, icoBuf);
                    // Text in default color
                    if (sp)
                    {
                        float icoW = ImGui::CalcTextSize(icoBuf).x;
                        dlIco->AddText(ImVec2(selPos.x + icoW + ImGui::CalcTextSize(" ").x, textY),
                            ImGui::GetColorU32(ImGuiCol_Text), sp + 1);
                    }
                }
                else
                {
                    // Default = gray (TextDisabled), accent on hover. Matches Groups/Tags rows.
                    ImVec4 colInactive = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                    ImGui::PushStyleColor(ImGuiCol_Text, hovered ? accentCol : colInactive);
                    if (ImGui::Selectable(lbl, false, 0, ImVec2(fullW, rowH)))
                        s.sort_mode = f.sort_val;
                    ImGui::PopStyleColor();
                }
                // Badge count (right-aligned, skip "Recent" which has no precomputed count)
                if (filterCounts[fi] >= 0)
                {
                    char badge[16];
                    snprintf(badge, sizeof(badge), "%d", filterCounts[fi]);
                    ImVec2 bSz = ImGui::CalcTextSize(badge);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 rMin = ImGui::GetItemRectMin();
                    ImVec2 rMax = ImGui::GetItemRectMax();
                    dl->AddText(render::FontSmall, render::FontSmall ? render::FontSmall->LegacySize : bSz.y,
                        ImVec2(sbContentRight - bSz.x - sbPad, rMin.y + (rowH - bSz.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), badge);
                }
                // (stale pop removed — split rendering handles its own push/pop)
            }

            ImGui::Spacing();

            // --- TYPES section header (collapsible) ---
            {
                const char* chevron = s.sidebar_types_collapsed ? ICON_MDI_CHEVRON_RIGHT : ICON_MDI_CHEVRON_DOWN;
                ImGui::PushFont(render::FontSmall);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                char typesHeader[64];
                snprintf(typesHeader, sizeof(typesHeader), "%s  TYPES", chevron);
                if (ImGui::Selectable(typesHeader, false, 0, ImVec2(fullW, rowH)))
                    s.sidebar_types_collapsed = !s.sidebar_types_collapsed;
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }

            if (!s.sidebar_types_collapsed)
            {
                // Check if any type filter is active
                bool anyTypeActive = false;
                for (const auto& key : s.selected_groups)
                    if (!key.empty() && key[0] == '@') { anyTypeActive = true; break; }

                if (anyTypeActive)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::Selectable(ICON_MDI_CLOSE "  Clear Types", false, 0, ImVec2(fullW, rowH)))
                    {
                        std::erase_if(s.selected_groups, [](const std::string& k) {
                            return !k.empty() && k[0] == '@';
                        });
                    }
                    ImGui::PopStyleColor();
                }

                // Type filters — outline when inactive, filled when active
                struct TypeEntry { const char* label; const char* label_active; const char* group_key; CredType type; };
                TypeEntry types[] = {
                    { ICON_MDI_KEY "  Passwords",          ICON_MDI_KEY "  Passwords",          "@Passwords",  CredType::Password },
                    { ICON_MDI_CREDIT_CARD "  Cards",      ICON_MDI_CREDIT_CARD "  Cards",      "@Cards",      CredType::CreditCard },
                    { ICON_MDI_CARD_ACCOUNT_DETAILS "  Identity", ICON_MDI_CARD_ACCOUNT_DETAILS "  Identity", "@Identity",   CredType::Identity },
                    { ICON_MDI_NOTE_TEXT "  Notes",        ICON_MDI_NOTE_TEXT "  Notes",         "@Notes",      CredType::SecureNote },
                };
                int typeCounts[] = { countPasswords, countCards, countIdentity, countNotes };
                for (int ti = 0; ti < 4; ti++)
                {
                    auto& t = types[ti];
                    bool active = s.selected_groups.count(t.group_key) > 0;
                    const char* lbl = active ? t.label_active : t.label;
                    ImVec2 curPos = ImGui::GetCursorScreenPos();
                    bool hovered = ImGui::IsMouseHoveringRect(curPos, ImVec2(curPos.x + fullW, curPos.y + rowH));

                    // Active indicator + rounded highlight (full width)
                    if (active)
                    {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        float hlLeft = ImGui::GetWindowPos().x;
                        dl->AddLine(
                            ImVec2(hlLeft, curPos.y),
                            ImVec2(sbContentRight, curPos.y),
                            dark ? IM_COL32(0, 0, 0, 30) : IM_COL32(0, 0, 0, 18), 1.0f);
                        dl->AddLine(
                            ImVec2(hlLeft, curPos.y + rowH),
                            ImVec2(sbContentRight, curPos.y + rowH),
                            dark ? IM_COL32(0, 0, 0, 50) : IM_COL32(0, 0, 0, 30), 1.0f);
                        ImU32 hlCol = dark ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 140);
                        dl->AddRectFilled(
                            ImVec2(hlLeft, curPos.y),
                            ImVec2(sbContentRight, curPos.y + rowH),
                            hlCol, 0.0f);
                    }

                    if (active)
                    {
                        const char* sp = strchr(lbl, ' ');
                        ImVec2 selPos = ImGui::GetCursorScreenPos();
                        ImGui::PushID(ti);
                        ImGui::InvisibleButton("##tsel", ImVec2(fullW, rowH));
                        bool clicked = ImGui::IsItemClicked();
                        ImGui::PopID();
                        if (clicked) { if (active) s.selected_groups.erase(t.group_key); else s.selected_groups.insert(t.group_key); }
                        ImDrawList* dlT = ImGui::GetWindowDrawList();
                        float textY = selPos.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
                        char icoBuf[64];
                        int icoLen = sp ? (int)(sp - lbl) : (int)strlen(lbl);
                        if (icoLen >= (int)sizeof(icoBuf)) icoLen = (int)sizeof(icoBuf) - 1;
                        memcpy(icoBuf, lbl, icoLen); icoBuf[icoLen] = 0;
                        dlT->AddText(ImVec2(selPos.x, textY), accentU32, icoBuf);
                        if (sp)
                        {
                            float icoW = ImGui::CalcTextSize(icoBuf).x;
                            dlT->AddText(ImVec2(selPos.x + icoW + ImGui::CalcTextSize(" ").x, textY),
                                ImGui::GetColorU32(ImGuiCol_Text), sp + 1);
                        }
                    }
                    else
                    {
                        // Default = gray (TextDisabled), accent on hover.
                        ImVec4 colInactive = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                        ImGui::PushStyleColor(ImGuiCol_Text, hovered ? accentCol : colInactive);
                        if (ImGui::Selectable(lbl, false, 0, ImVec2(fullW, rowH)))
                            s.selected_groups.insert(t.group_key);
                        ImGui::PopStyleColor();
                    }
                    if (typeCounts[ti] >= 0)
                    {
                        char badge[16];
                        snprintf(badge, sizeof(badge), "%d", typeCounts[ti]);
                        ImVec2 bSz = ImGui::CalcTextSize(badge);
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        ImVec2 rMin = ImGui::GetItemRectMin();
                        ImVec2 rMax = ImGui::GetItemRectMax();
                        dl->AddText(render::FontSmall, render::FontSmall ? render::FontSmall->LegacySize : bSz.y,
                            ImVec2(sbContentRight - bSz.x - sbPad, rMin.y + (rowH - bSz.y) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), badge);
                    }
                }
            }

            ImGui::Spacing();

            // --- GROUPS section header (collapsible) ---
            {
                const char* chevron = s.sidebar_groups_collapsed ? ICON_MDI_CHEVRON_RIGHT : ICON_MDI_CHEVRON_DOWN;
                ImGui::PushFont(render::FontSmall);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                char groupsHeader[64];
                snprintf(groupsHeader, sizeof(groupsHeader), "%s  GROUPS", chevron);
                if (ImGui::Selectable(groupsHeader, false, 0, ImVec2(fullW, rowH)))
                    s.sidebar_groups_collapsed = !s.sidebar_groups_collapsed;
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }

            if (!s.sidebar_groups_collapsed)
            {
                // Groups container: shrink to actual content height (capped at 12 rows)
                // so the inline Tags section sits right below Groups instead of being
                // pushed to the sidebar bottom. Longer group lists scroll inside the
                // child rather than expanding it.
                const float maxGroupsH = rowH * 12.0f;
                int visibleCount = 0;
                for (const auto& g : s.groups) {
                    if (g == "Filter" || g == "All" || g == "---") continue;
                    if (!g.empty() && g[0] == '@') continue;
                    visibleCount++;
                }
                float contentH = visibleCount * (rowH + 1.0f) + 4.0f;
                float groupsMaxH = ImMin(maxGroupsH, contentH);
                if (groupsMaxH < rowH * 2.0f) groupsMaxH = rowH * 2.0f;
                ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0,0,0,0));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                ImGui::BeginChild("##sb_groups_scroll", ImVec2(0, groupsMaxH), false);
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();

                // Check if any user group filter is active
                bool anyGroupActive = false;
                for (const auto& key : s.selected_groups)
                    if (key.empty() || key[0] != '@') { anyGroupActive = true; break; }

                if (anyGroupActive)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::Selectable(ICON_MDI_CLOSE "  Clear Groups", false, 0, ImVec2(fullW, rowH)))
                    {
                        std::erase_if(s.selected_groups, [](const std::string& k) {
                            return k.empty() || k[0] != '@';
                        });
                    }
                    ImGui::PopStyleColor();
                }

                // User groups
                for (const auto& g : s.groups)
                {
                    if (g == "Filter" || g == "All") continue;
                    if (!g.empty() && g[0] == '@') continue;
                    if (g == "---") continue;

                    bool active = s.selected_groups.count(g) > 0;
                    const char* ico = active ? ICON_MDI_FOLDER : ICON_MDI_FOLDER;

                    ImGui::PushID(g.c_str());
                    char label[256];
                    snprintf(label, sizeof(label), "%s  %s", ico, g.c_str());
                    ImVec2 curPos = ImGui::GetCursorScreenPos();
                    bool hovered = ImGui::IsMouseHoveringRect(curPos, ImVec2(curPos.x + fullW, curPos.y + rowH));

                    // Active indicator bar + highlight (full width)
                    if (active)
                    {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        float hlLeft = ImGui::GetWindowPos().x;
                        dl->AddLine(
                            ImVec2(hlLeft, curPos.y),
                            ImVec2(sbContentRight, curPos.y),
                            dark ? IM_COL32(0, 0, 0, 30) : IM_COL32(0, 0, 0, 18), 1.0f);
                        dl->AddLine(
                            ImVec2(hlLeft, curPos.y + rowH),
                            ImVec2(sbContentRight, curPos.y + rowH),
                            dark ? IM_COL32(0, 0, 0, 50) : IM_COL32(0, 0, 0, 30), 1.0f);
                        ImU32 hlCol = dark ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 140);
                        dl->AddRectFilled(
                            ImVec2(hlLeft, curPos.y),
                            ImVec2(sbContentRight, curPos.y + rowH),
                            hlCol, 0.0f);
                    }

                    if (active)
                    {
                        const char* sp = strchr(label, ' ');
                        ImVec2 selPos = ImGui::GetCursorScreenPos();
                        ImGui::InvisibleButton("##gsel", ImVec2(fullW, rowH));
                        bool clicked = ImGui::IsItemClicked();
                        if (clicked) s.selected_groups.erase(g);
                        ImDrawList* dlG = ImGui::GetWindowDrawList();
                        float textY = selPos.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
                        char icoBuf[64];
                        int icoLen = sp ? (int)(sp - label) : (int)strlen(label);
                        if (icoLen >= (int)sizeof(icoBuf)) icoLen = (int)sizeof(icoBuf) - 1;
                        memcpy(icoBuf, label, icoLen); icoBuf[icoLen] = 0;
                        dlG->AddText(ImVec2(selPos.x, textY), accentU32, icoBuf);
                        if (sp)
                        {
                            float icoW = ImGui::CalcTextSize(icoBuf).x;
                            dlG->AddText(ImVec2(selPos.x + icoW + ImGui::CalcTextSize(" ").x, textY),
                                ImGui::GetColorU32(ImGuiCol_Text), sp + 1);
                        }
                    }
                    else
                    {
                        // Default = gray, accent on hover (matches Windows sidebar look)
                        ImVec4 colInactive = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                        ImGui::PushStyleColor(ImGuiCol_Text, hovered ? accentCol : colInactive);
                        if (ImGui::Selectable(label, false, 0, ImVec2(fullW, rowH)))
                        {
                            // Toggle expand/collapse inline items
                            if (s.sidebar_expanded_groups.count(g))
                                s.sidebar_expanded_groups.erase(g);
                            else
                                s.sidebar_expanded_groups.insert(g);
                        }
                        ImGui::PopStyleColor();
                    }

                    // Drop target: credential → group
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CRED_ID"))
                        {
                            s.drag_drop_cred_id = *(int*)payload->Data;
                            s.drag_drop_target_group = g;
                        }
                        ImGui::EndDragDropTarget();
                    }

                    // Badge count + chevron indicator
                    {
                        bool expanded = s.sidebar_expanded_groups.count(g) > 0;
                        const char* chevronIco = expanded ? ICON_MDI_CHEVRON_DOWN : ICON_MDI_CHEVRON_RIGHT;
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        ImVec2 rMin = ImGui::GetItemRectMin();

                        // Show count badge
                        auto gcIt = groupCounts.find(g);
                        int gc = gcIt != groupCounts.end() ? gcIt->second : 0;
                        if (gc > 0)
                        {
                            char badge[16];
                            snprintf(badge, sizeof(badge), "%d", gc);
                            ImVec2 bSz = ImGui::CalcTextSize(badge);
                            ImVec2 chevSz = ImGui::CalcTextSize(chevronIco);
                            float badgeX = sbContentRight - bSz.x - sbPad - chevSz.x - 4.0f;
                            dl->AddText(render::FontSmall, render::FontSmall ? render::FontSmall->LegacySize : bSz.y,
                                ImVec2(badgeX, rMin.y + (rowH - bSz.y) * 0.5f),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), badge);
                        }

                        // Chevron
                        ImVec2 chevSz = ImGui::CalcTextSize(chevronIco);
                        float chevX = sbContentRight - chevSz.x - sbPad;
                        float chevY = rMin.y + (rowH - chevSz.y) * 0.5f;
                        dl->AddText(ImVec2(chevX, chevY),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), chevronIco);
                    }

                    // Inline credential items when group is expanded
                    if (s.sidebar_expanded_groups.count(g) > 0)
                    {
                        auto git = s.sb_group_items.find(g);
                        if (git != s.sb_group_items.end())
                        {
                            ImGui::Indent(12.0f);
                            const float childRowH = 24.0f;
                            const float icoSz = 16.0f;
                            static std::unordered_set<std::string> s_show_all_groups;
                            bool showAll = s_show_all_groups.count(g) > 0;
                            int maxItems = showAll ? (int)git->second.size() : 20;
                            int totalToShow = (int)git->second.size() < maxItems ? (int)git->second.size() : maxItems;

                            // Tree line X position (left of indent)
                            float treeLineX = ImGui::GetCursorScreenPos().x - 6.0f;
                            ImU32 treeCol = dark ? IM_COL32(255, 255, 255, 25) : IM_COL32(0, 0, 0, 18);

                            int shown = 0;
                            for (const auto& child : git->second)
                            {
                                if (shown >= maxItems) break;
                                ImGui::PushID(child.id);
                                bool childSelected = (s.three_pane_selected_id == child.id);
                                bool isLast = (shown == totalToShow - 1);

                                char childLabel[64];
                                snprintf(childLabel, sizeof(childLabel), "##sbc_%d", child.id);
                                ImVec2 selPos = ImGui::GetCursorScreenPos();

                                ImGui::PushStyleColor(ImGuiCol_SliderGrab, colors::SecondColor);
                                if (ImGui::Selectable3(childLabel, childSelected, 0, ImVec2(fullW - sbPad - 12.0f, childRowH)))
                                {
                                    s.three_pane_selected_id = child.id;
                                    s.recent_touch_uuid = child.uuid;
                                }
                                ImGui::PopStyleColor();

                                // Draw favicon + title
                                {
                                    ImDrawList* cdl = ImGui::GetWindowDrawList();
                                    float iconX = selPos.x + 4.0f;
                                    float iconY = selPos.y + (childRowH - icoSz) * 0.5f;

                                    auto srv = favicon::Get(child.website);
                                    if (srv)
                                    {
                                        cdl->AddImageRounded((ImTextureID)srv,
                                            ImVec2(iconX, iconY), ImVec2(iconX + icoSz, iconY + icoSz),
                                            ImVec2(0,0), ImVec2(1,1), IM_COL32(255,255,255,255), 3.0f);
                                    }
                                    else
                                    {
                                        const char* tIco = CredTypeIcon(child.type);
                                        ImVec2 tSz = ImGui::CalcTextSize(tIco);
                                        cdl->AddText(ImVec2(iconX, selPos.y + (childRowH - tSz.y) * 0.5f),
                                            ImGui::GetColorU32(childSelected ? ImGuiCol_Text : ImGuiCol_TextDisabled), tIco);
                                    }

                                    // Title
                                    float textX = iconX + icoSz + 4.0f;
                                    float textY = selPos.y + (childRowH - ImGui::GetTextLineHeight()) * 0.5f;
                                    float maxTextW = fullW - sbPad - 32.0f;
                                    ImGui::PushClipRect(ImVec2(textX, selPos.y), ImVec2(textX + maxTextW, selPos.y + childRowH), true);
                                    cdl->AddText(ImVec2(textX, textY),
                                        ImGui::GetColorU32(childSelected ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                                        DisplayTitle(child.title));
                                    ImGui::PopClipRect();
                                }

                                // Tree branch: horizontal tick from vertical line to icon
                                {
                                    ImDrawList* tdl = ImGui::GetWindowDrawList();
                                    float midY = selPos.y + childRowH * 0.5f;
                                    // Horizontal branch
                                    tdl->AddLine(ImVec2(treeLineX, midY), ImVec2(treeLineX + 5.0f, midY), treeCol, 1.0f);
                                    // Vertical line segment (from top of this row to midpoint, or full row if not last)
                                    float vTop = selPos.y;
                                    float vBot = isLast ? midY : (selPos.y + childRowH);
                                    tdl->AddLine(ImVec2(treeLineX, vTop), ImVec2(treeLineX, vBot), treeCol, 1.0f);
                                }

                                if (ImGui::IsItemHovered()) SetTooltipPadded("%s", DisplayTitle(child.title));
                                ImGui::PopID();
                                shown++;
                            }
                            if (!showAll && (int)git->second.size() > 20)
                            {
                                char moreBuf[32];
                                snprintf(moreBuf, sizeof(moreBuf), "+%d more", (int)git->second.size() - 20);
                                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                                if (ImGui::Selectable(moreBuf, false, 0, ImVec2(fullW - sbPad - 12.0f, childRowH)))
                                    s_show_all_groups.insert(g);
                                ImGui::PopStyleColor();
                            }
                            else if (showAll && (int)git->second.size() > 20)
                            {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                                if (ImGui::Selectable("Show less", false, 0, ImVec2(fullW - sbPad - 12.0f, childRowH)))
                                    s_show_all_groups.erase(g);
                                ImGui::PopStyleColor();
                            }
                            ImGui::Unindent(12.0f);
                        }
                    }

                    ImGui::PopID();
                }

                ImGui::EndChild(); // ##sb_groups_scroll
            }
            ImGui::Unindent(sbPad);

            // --- TAGS section (expanded sidebar, inline list) ---
            // Replaces the prior button+popup with Windows-style inline rendering.
            if (!s.all_tags.empty())
            {
                ImGui::Spacing();   // single spacer — double pushed tags too far below groups
                ImGui::Indent(sbPad);

                // Chevron header "TAGS"
                {
                    const char* chevron = s.sidebar_tags_collapsed ? ICON_MDI_CHEVRON_RIGHT : ICON_MDI_CHEVRON_DOWN;
                    ImGui::PushFont(render::FontSmall);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    char tagsHeader[64];
                    snprintf(tagsHeader, sizeof(tagsHeader), "%s  TAGS", chevron);
                    if (ImGui::Selectable(tagsHeader, false, 0, ImVec2(fullW, rowH)))
                        s.sidebar_tags_collapsed = !s.sidebar_tags_collapsed;
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                }

                if (!s.sidebar_tags_collapsed)
                {
                    bool anyTagActive = !s.selected_tags.empty();
                    if (anyTagActive)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        if (ImGui::Selectable(ICON_MDI_CLOSE "  Clear Tags", false, 0, ImVec2(fullW, rowH)))
                            s.selected_tags.clear();
                        ImGui::PopStyleColor();
                    }

                    // Scrollable tag list (caps height so it doesn't push other sections off-screen)
                    float tagsMaxH = ImGui::GetContentRegionAvail().y - (rowH + 8.0f);
                    if (tagsMaxH < 60.0f) tagsMaxH = 60.0f;
                    ImGui::PushStyleColor(ImGuiCol_ChildBg,              IM_COL32(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          IM_COL32(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        IM_COL32(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  IM_COL32(0, 0, 0, 0));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                    // Capture the sidebar (outer) window left BEFORE entering the tag child,
                    // so active-tag highlights can bleed full width like the TYPES rows.
                    float sbWinLeft = ImGui::GetWindowPos().x;
                    ImGui::BeginChild("##sb_tags_scroll", ImVec2(0, tagsMaxH), false);
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(5);

                    for (const auto& tag : s.all_tags)
                    {
                        bool active = s.selected_tags.count(tag) > 0;
                        const char* ico = ICON_MDI_TAG;
                        auto tcIt = s.sb_tag_counts.find(tag);
                        int tc = tcIt != s.sb_tag_counts.end() ? tcIt->second : 0;

                        char label[256];
                        snprintf(label, sizeof(label), "%s  %s", ico, tag.c_str());
                        ImVec2 curPos = ImGui::GetCursorScreenPos();
                        bool hovered = ImGui::IsMouseHoveringRect(curPos, ImVec2(curPos.x + fullW, curPos.y + rowH));

                        // Active row highlight (matches Groups section style)
                        if (active)
                        {
                            ImDrawList* dl_t = ImGui::GetWindowDrawList();
                            float hlLeft = sbWinLeft; // bleed to the sidebar's left edge, like TYPES rows
                            // Tag rows live in a scroll child whose clip rect starts at the indented
                            // content; widen it for this row so the highlight reaches both the far
                            // left and sbContentRight, matching the TYPES highlight exactly.
                            dl_t->PushClipRect(ImVec2(hlLeft, curPos.y),
                                               ImVec2(sbContentRight + 1.0f, curPos.y + rowH + 1.0f), false);
                            dl_t->AddLine(
                                ImVec2(hlLeft, curPos.y),
                                ImVec2(sbContentRight, curPos.y),
                                dark ? IM_COL32(0, 0, 0, 30) : IM_COL32(0, 0, 0, 18), 1.0f);
                            dl_t->AddLine(
                                ImVec2(hlLeft, curPos.y + rowH),
                                ImVec2(sbContentRight, curPos.y + rowH),
                                dark ? IM_COL32(0, 0, 0, 50) : IM_COL32(0, 0, 0, 30), 1.0f);
                            ImU32 hlCol = dark ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 140);
                            dl_t->AddRectFilled(
                                ImVec2(hlLeft, curPos.y),
                                ImVec2(sbContentRight, curPos.y + rowH),
                                hlCol, 0.0f);
                            dl_t->PopClipRect();
                        }

                        ImGui::PushID(tag.c_str());
                        {
                            ImVec4 colInactive = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                            ImGui::PushStyleColor(ImGuiCol_Text, (active || hovered) ? accentCol : colInactive);
                            if (ImGui::Selectable(label, active, 0, ImVec2(fullW, rowH)))
                            {
                                if (active) s.selected_tags.erase(tag);
                                else        s.selected_tags.insert(tag);
                            }
                            ImGui::PopStyleColor();
                        }
                        ImGui::PopID();

                        // Badge count (right-aligned)
                        if (tc > 0)
                        {
                            char badge[16];
                            snprintf(badge, sizeof(badge), "%d", tc);
                            ImVec2 bSz = ImGui::CalcTextSize(badge);
                            ImDrawList* dl_b = ImGui::GetWindowDrawList();
                            ImVec2 rMin = ImGui::GetItemRectMin();
                            dl_b->AddText(
                                ImVec2(sbContentRight - bSz.x - sbPad, rMin.y + (rowH - bSz.y) * 0.5f),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), badge);
                        }
                    }

                    ImGui::EndChild(); // ##sb_tags_scroll
                }

                ImGui::Unindent(sbPad);
            }

        }

        ImGui::PopStyleVar();    // SelectableTextAlign
        ImGui::PopStyleColor(3); // Header, HeaderHovered, HeaderActive

        float sbScrollY = ImGui::GetScrollY();
        ImVec2 sbScrollMin = ImGui::GetWindowPos();
        float sbScrollW = ImGui::GetWindowSize().x;
        ImGui::EndChild(); // ##3p_sb_scroll
        // DrawScrollTopFade(sbScrollMin, sbScrollW, sbScrollY, -3.0f);

        EndLippedChild(); // ##3p_sidebar
    }

    static void RenderThreePaneMiddleList(
        const std::vector<AccordionItem>& items,
        ShellState& s,
        uint32_t activeVaultKey,
        float animW)
    {
        const bool collapsed = s.three_pane_list_collapsed;
        const float listW = animW;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 8));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0,0,0,0));
        ImGui::BeginChild("##3p_list", ImVec2(listW, 0), ImGuiChildFlags_Borders);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        const ImU32 accentU32 = ImGui::GetColorU32(colors::SecondColor);
        float fullW = ImGui::GetContentRegionAvail().x;
        const float toggleCenterW = collapsed ? 56.0f : fullW; // fixed centering during animation

        // Toggle button + heading
        {
            const float mlPad = 17.0f;
            const char* toggleIcon = collapsed ? ICON_MDI_CHEVRON_RIGHT : ICON_MDI_CHEVRON_LEFT;
            float btnSz = 20.0f;
            if (collapsed)
            {
                float cx = (toggleCenterW - btnSz) * 0.5f;
                if (cx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
            }
            else
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + mlPad);
            }
            float btnY = ImGui::GetCursorPosY();
            if (IconButtonDoubleLip("##ml_toggle", toggleIcon, btnSz))
            {
                s.three_pane_list_collapsed = !collapsed;
                cfg::set_three_pane_list_collapsed(s.three_pane_list_collapsed);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("##ml_auto_popup");
            if (!collapsed)
            {
                ImGui::SameLine(0, 6);
                float textOffY = (btnSz - ImGui::GetTextLineHeight()) * 0.5f;
                ImGui::SetCursorPosY(btnY + textOffY);

                std::string heading;
                if (s.sort_mode == 0)       heading = s.pill_tab_0;
                else if (s.sort_mode == 1)  heading = s.pill_tab_1;
                else if (s.sort_mode == 3)  heading = "Recent";

                if (!s.selected_groups.empty())
                {
                    int shown = 0;
                    for (const auto& g : s.selected_groups)
                    {
                        if (shown >= 5)
                        {
                            int remaining = (int)s.selected_groups.size() - shown;
                            heading += " +" + std::to_string(remaining);
                            break;
                        }
                        if (!heading.empty()) heading += ", ";
                        if (g == "@Passwords")       heading += "Passwords";
                        else if (g == "@Cards")      heading += "Cards";
                        else if (g == "@Identity")   heading += "Identity";
                        else if (g == "@Notes")      heading += "Notes";
                        else                         heading += g;
                        shown++;
                    }
                }

                if (!s.selected_tags.empty())
                {
                    int shown = 0;
                    for (const auto& t : s.selected_tags)
                    {
                        if (shown >= 3)
                        {
                            int remaining = (int)s.selected_tags.size() - shown;
                            heading += " +" + std::to_string(remaining);
                            break;
                        }
                        if (heading.empty())
                            heading = t;
                        else
                            heading += ", " + t;
                        shown++;
                    }
                }

                if (heading.empty()) heading = "All Items";

                char headingBuf[256];
                int credCount = 0;
                for (const auto& it : items) { if (!it.is_header) credCount++; }
                snprintf(headingBuf, sizeof(headingBuf), "%s (%d)", heading.c_str(), credCount);
                ImGui::TextDisabled("%s", headingBuf);
            }

            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,  8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, IsDarkTheme() ? IM_COL32(26, 26, 28, 255) : IM_COL32(250, 250, 252, 255));
            ImGui::PushStyleColor(ImGuiCol_Border,  IsDarkTheme() ? IM_COL32(255,255,255,25) : IM_COL32(0,0,0,30));
            if (ImGui::BeginPopup("##ml_auto_popup"))
            {
                PopupStyleBegin();
                ImGui::TextDisabled("Auto-collapse");
                ImGui::Separator();
                bool mlAuto = s.three_pane_list_auto_collapse;
                if (ImGui::Checkbox3("Item List", &mlAuto))
                {
                    s.three_pane_list_auto_collapse = mlAuto;
                    cfg::set_three_pane_list_auto_collapse(mlAuto);
                    if (mlAuto)
                    {
                        s.three_pane_list_collapsed = true;
                        cfg::set_three_pane_list_collapsed(true);
                    }
                    else
                    {
                        g_3p_list_hover_expanded = false;
                        g_3p_list_leave_timer = 0.0f;
                    }
                }
                PopupStyleEnd();
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);

            ImGui::Spacing();
        }

        // Scrollable content area (header stays fixed above)
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(0,0,0,0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
        ImGui::BeginChild("##3p_list_scroll", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        fullW = ImGui::GetContentRegionAvail().x;

        if (collapsed)
        {
            // ---- COLLAPSED MODE: favicon/icon strip ----
            const float iconW = fullW;
            const float centerW = iconW; // center icons within available width
            const float icoSz = 24.0f;
            const float rowH = 38.0f;

            for (const auto& it : items)
            {
                if (it.is_header)
                {
                    // Compact group separator: first letter in parens, centered
                    // e.g. "December 2025" → "(D)", "Banking" → "(B)", "A" → "(A)"
                    char badge[8];
                    if (!it.header_label.empty())
                        snprintf(badge, sizeof(badge), "%c", it.header_label[0]);
                    else
                        snprintf(badge, sizeof(badge), "-");
                    ImVec2 bSz = ImGui::CalcTextSize(badge);
                    float cx = (centerW - bSz.x) * 0.5f;
                    if (cx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
                    ImGui::TextDisabled("%s", badge);
                    if (ImGui::IsItemHovered()) SetTooltipPadded("%s", it.header_label.c_str());
                    continue;
                }

                ImGui::PushID((int)MakeRowKey(activeVaultKey, it.id));

                bool selected = (s.three_pane_selected_id == it.id);
                bool hovered_row = false;

                char label[64];
                snprintf(label, sizeof(label), "##cl_%d", it.id);

                float selW = rowH;
                float indent = (iconW - selW) * 0.5f;
                if (indent > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
                const uint64_t rowKey = MakeRowKey(activeVaultKey, it.id);
                ImVec2 selPos = ImGui::GetCursorScreenPos();
                ImGui::PushStyleColor(ImGuiCol_SliderGrab, colors::SecondColor);
                if (ImGui::Selectable3(label, selected, 0, ImVec2(selW, rowH)))
                {
                    if (ImGui::GetIO().KeyCtrl)
                        ToggleSelected(rowKey);
                    else
                    {
                        s.three_pane_selected_id = it.id;
                        s.recent_touch_uuid = it.uuid;
                    }
                }
                ImGui::PopStyleColor();

                // Drag source for credential → group
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                {
                    ImGui::SetDragDropPayload("CRED_ID", &it.id, sizeof(int));
                    ImGui::Text("%s", DisplayTitle(it.title));
                    ImGui::EndDragDropSource();
                }

                hovered_row = ImGui::IsItemHovered();

                // Multi-select highlight
                if (IsSelected(rowKey))
                {
                    ImVec2 rMin = ImGui::GetItemRectMin();
                    ImVec2 rMax = ImGui::GetItemRectMax();
                    bool dk = IsDarkTheme();
                    ImU32 multiSelTint = dk ? IM_COL32(colors::SecondColor.x * 255, colors::SecondColor.y * 255, colors::SecondColor.z * 255, 35)
                                            : IM_COL32(colors::SecondColor.x * 255, colors::SecondColor.y * 255, colors::SecondColor.z * 255, 25);
                    ImGui::GetWindowDrawList()->AddRectFilled(rMin, rMax, multiSelTint, 4.0f);
                }

                if (hovered_row)
                {
                    if (s.hover_expand)
                        s.three_pane_selected_id = it.id;
                    SetTooltipPadded("%s", DisplayTitle(it.title));
                }

                // Draw favicon or type icon centered in row
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    auto srv = favicon::Get(it.website);
                    const float drawSz = icoSz;

                    if (srv)
                    {
                        float iconX = selPos.x + (selW - drawSz) * 0.5f;
                        float iconY = selPos.y + (rowH - drawSz) * 0.5f;
                        dl->AddImage((ImTextureID)srv,
                            ImVec2(iconX, iconY), ImVec2(iconX + drawSz, iconY + drawSz));
                    }
                    else
                    {
                        const char* ico = CredTypeIcon(it.type);
                        ImVec2 txtSz = ImGui::CalcTextSize(ico);
                        float iconX = selPos.x + (selW - txtSz.x) * 0.5f;
                        float iconY = selPos.y + (rowH - txtSz.y) * 0.5f;
                        ImU32 icoCol = selected ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
                        dl->AddText(ImVec2(iconX, iconY), icoCol, ico);
                    }
                }

                // Changed-entry tint overlay
                if (it.is_changed)
                {
                    ImVec2 rMin = ImGui::GetItemRectMin();
                    ImVec2 rMax = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddRectFilled(rMin, rMax,
                        (it.changed_fields & FCF_IsNew) ? kNewRowTint : kChangedRowTint);
                }

                ImGui::PopID();

                ImGui::Dummy(ImVec2(0, 2.0f));

                // Row divider with shadow lip (centered with icons)
                {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    float winX = ImGui::GetWindowPos().x;
                    float x0 = winX + (iconW - rowH) * 0.5f;
                    float x1 = x0 + rowH;
                    float y = p.y - 2.0f;
                    bool dark = IsDarkTheme();
                    dl->AddLine(ImVec2(x0, y), ImVec2(x1, y),
                        dark ? IM_COL32(0, 0, 0, 30) : IM_COL32(0, 0, 0, 18), 1.0f);
                    dl->AddLine(ImVec2(x0, y + 1.0f), ImVec2(x1, y + 1.0f),
                        dark ? IM_COL32(255, 255, 255, 8) : IM_COL32(255, 255, 255, 120), 1.0f);
                }
            }
        }
        else
        {
            // ---- EXPANDED MODE (existing code) ----
            ImGui::Indent(8.0f);

            bool tpSkipUntilNextHeader = false;
            for (const auto& it : items)
            {
                if (it.is_header)
                {
                    ImGui::Spacing();
                    bool hdrCollapsed = g_collapsed_headers.count(it.header_label) > 0;
                    const char* chevron = hdrCollapsed ? ICON_MDI_CHEVRON_RIGHT : ICON_MDI_CHEVRON_DOWN;
                    ImGui::PushID(it.header_label.c_str());
                    ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(0,0,0,0));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  IM_COL32(0,0,0,0));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   IM_COL32(0,0,0,0));
                    float hdrW = ImGui::GetContentRegionAvail().x;
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::Selectable(it.header_label.c_str(), false, 0, ImVec2(hdrW, 0)))
                    {
                        if (hdrCollapsed) g_collapsed_headers.erase(it.header_label);
                        else              g_collapsed_headers.insert(it.header_label);
                        hdrCollapsed = !hdrCollapsed;
                    }
                    ImGui::PopStyleColor();
                    // Draw chevron on far right
                    ImVec2 chevSz = ImGui::CalcTextSize(chevron);
                    ImVec2 rMin = ImGui::GetItemRectMin();
                    ImVec2 rMax = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(rMax.x - chevSz.x - 4.0f, rMin.y + (rMax.y - rMin.y - chevSz.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), chevron);
                    ImGui::PopStyleColor(3);
                    ImGui::PopID();
                    tpSkipUntilNextHeader = hdrCollapsed;
                    continue;
                }

                if (tpSkipUntilNextHeader) continue;

                ImGui::PushID((int)MakeRowKey(activeVaultKey, it.id));

                bool selected = (s.three_pane_selected_id == it.id);

                // Build subtitle based on type + per-type toggles
                const auto& cols = s.detailed_header_cols;
                std::string subtitle;
                switch (it.type)
                {
                case CredType::Password:
                    if (cols.sub_password) {
                        if (cols.email && !it.email.empty())       subtitle = MaskIfPrivate(it.email) ? MaskIfPrivate(it.email) : it.email;
                        else if (cols.username && !it.user.empty()) subtitle = MaskIfPrivate(it.user) ? MaskIfPrivate(it.user) : it.user;
                    }
                    break;
                case CredType::CreditCard:
                    if (cols.sub_card) {
                        if (it.card_number.size() >= 4)
                            subtitle = std::string("****") + it.card_number.substr(it.card_number.size() - 4);
                        else
                            subtitle = MaskIfPrivate(it.cardholder_name) ? MaskIfPrivate(it.cardholder_name) : it.cardholder_name;
                    }
                    break;
                case CredType::Identity:
                    if (cols.sub_identity)
                        subtitle = MaskIfPrivate(it.full_name) ? MaskIfPrivate(it.full_name) : it.full_name;
                    break;
                case CredType::SecureNote:
                    if (cols.sub_note)
                        subtitle = it.notes.substr(0, 40);
                    break;
                }

                // Title line in regular font, subtitle in small font
                ImGui::PushFont(render::FontRegular);
                auto srv = favicon::Get(it.website);
                char label[64];
                snprintf(label, sizeof(label), "##3p_%d", it.id);

                const uint64_t rowKey = MakeRowKey(activeVaultKey, it.id);
                ImVec2 selPos = ImGui::GetCursorScreenPos();
                ImGui::PushStyleColor(ImGuiCol_SliderGrab, colors::SecondColor);
                if (ImGui::Selectable3(label, selected, 0, ImVec2(fullW - 16.0f, 38)))
                {
                    if (ImGui::GetIO().KeyCtrl)
                        ToggleSelected(rowKey);
                    else
                    {
                        s.three_pane_selected_id = it.id;
                        s.recent_touch_uuid = it.uuid;
                    }
                }

                ImGui::PopStyleColor();

                // Drag source for credential → group
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                {
                    ImGui::SetDragDropPayload("CRED_ID", &it.id, sizeof(int));
                    ImGui::Text("%s", DisplayTitle(it.title));
                    ImGui::EndDragDropSource();
                }

                if (s.hover_expand && ImGui::IsItemHovered())
                    s.three_pane_selected_id = it.id;

                // Multi-select highlight
                if (IsSelected(rowKey))
                {
                    ImVec2 rMin = ImGui::GetItemRectMin();
                    ImVec2 rMax = ImGui::GetItemRectMax();
                    bool dk = IsDarkTheme();
                    ImU32 multiSelTint = dk ? IM_COL32(colors::SecondColor.x * 255, colors::SecondColor.y * 255, colors::SecondColor.z * 255, 35)
                                            : IM_COL32(colors::SecondColor.x * 255, colors::SecondColor.y * 255, colors::SecondColor.z * 255, 25);
                    ImGui::GetWindowDrawList()->AddRectFilled(rMin, rMax, multiSelTint, 4.0f);
                }

                bool hovered_exp = ImGui::IsItemHovered();

                // Draw favicon + vertically centered title/subtitle via DrawList
                {
                    ImDrawList* fdl = ImGui::GetWindowDrawList();
                    ImVec2 rMin = ImGui::GetItemRectMin();
                    float regSz  = render::FontBold ? render::FontBold->LegacySize : ImGui::GetTextLineHeight();
                    float subSz  = render::FontSmall ? render::FontSmall->LegacySize : ImGui::GetTextLineHeight();
                    float textX  = rMin.x + 44.0f;
                    const float rowH = 38.0f;
                    const float gap = 2.0f;

                    // Compute total text block height and center it
                    float blockH = regSz + (subtitle.empty() ? 0.0f : gap + subSz);
                    float blockY = rMin.y + (rowH - blockH) * 0.5f + 2.0f;

                    // Title (overwrite selectable's own text rendering with centered version)
                    {
                        const char* titleText = cols.title ? DisplayTitle(it.title) : "";
                        ImVec2 titlePos(textX, blockY);
                        fdl->AddText(render::FontBold, regSz, titlePos, ImGui::GetColorU32(ImGuiCol_Text), titleText);
                    }

                    // Subtitle (brighter when selected)
                    if (!subtitle.empty())
                    {
                        ImU32 subCol = selected
                            ? ImGui::GetColorU32(ImGuiCol_Text, 0.7f)
                            : ImGui::GetColorU32(ImGuiCol_TextDisabled);
                        fdl->AddText(render::FontSmall, subSz,
                            ImVec2(textX, blockY + regSz + gap), subCol, subtitle.c_str());
                    }

                    // Favicon or type icon fallback
                    const float icoSz = 24.0f;

                    if (srv)
                    {
                        float iconY = rMin.y + (rowH - icoSz) * 0.5f + 2.0f;
                        float iconX = rMin.x + 12.0f;
                        fdl->AddImage((ImTextureID)srv,
                            ImVec2(iconX, iconY), ImVec2(iconX + icoSz, iconY + icoSz));
                    }
                    else
                    {
                        const char* ico = CredTypeIcon(it.type);
                        float icoFontSz = render::FontRegular ? render::FontRegular->LegacySize : regSz;
                        float iconY = rMin.y + (rowH - icoFontSz) * 0.5f + 2.0f;
                        ImU32 icoCol = selected ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
                        fdl->AddText(render::FontRegular, icoFontSz,
                            ImVec2(rMin.x + 12.0f, iconY), icoCol, ico);
                    }
                }

                // Right-click context menu on row (only if no other popup is open)
                bool threePaneCtxAllowed = !ImGui::IsPopupOpen((const char*)NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)
                                        || ImGui::IsPopupOpen("##3p_list_ctx");
                ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,  8.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 8));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(4, 4));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(6, 2));
                ImGui::PushStyleColor(ImGuiCol_PopupBg, IsDarkTheme() ? IM_COL32(26, 26, 28, 255) : IM_COL32(250, 250, 252, 255));
                ImGui::PushStyleColor(ImGuiCol_Border,  IsDarkTheme() ? IM_COL32(255,255,255,25) : IM_COL32(0,0,0,30));

                if (threePaneCtxAllowed && ImGui::BeginPopupContextItem("##3p_list_ctx"))
                {
                    PopupStyleBegin();
                    ImGui::PushFont(render::FontSmall);
                    auto& mcols = s.detailed_header_cols;
                    if (BarMenuItemToggle("Show Title",    &mcols.title,    true, true)) cfg::set_detailed_header_columns(mcols);
                    if (BarMenuItemToggle("Show Username", &mcols.username, true, true)) cfg::set_detailed_header_columns(mcols);
                    if (BarMenuItemToggle("Show Email",    &mcols.email,    true, true)) cfg::set_detailed_header_columns(mcols);
                    if (BarMenuItemToggle("Show Pin/Fav",  &mcols.pin_fav,  true, true)) cfg::set_detailed_header_columns(mcols);
                    ImGui::Separator();
                    if (BarMenuItemToggle("Password Subtitle", &mcols.sub_password, true, true)) cfg::set_detailed_header_columns(mcols);
                    if (BarMenuItemToggle("Card Subtitle",     &mcols.sub_card,     true, true)) cfg::set_detailed_header_columns(mcols);
                    if (BarMenuItemToggle("Identity Subtitle", &mcols.sub_identity, true, true)) cfg::set_detailed_header_columns(mcols);
                    if (BarMenuItemToggle("Note Subtitle",     &mcols.sub_note,     true, true)) cfg::set_detailed_header_columns(mcols);
                    ImGui::PopFont();
                    PopupStyleEnd();
                    ImGui::EndPopup();
                }
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(5);

                ImGui::PopFont();

                // Changed-entry tint overlay
                if (it.is_changed)
                {
                    ImVec2 rMin = ImGui::GetItemRectMin();
                    ImVec2 rMax = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddRectFilled(rMin, rMax,
                        (it.changed_fields & FCF_IsNew) ? kNewRowTint : kChangedRowTint);
                }

                // Draw pin/fav + security icons on the right side of the selectable row.
                // Heart = red, pin = orange (accent) — matches Windows ui_views_threepane.cpp:1696/1703
                // and the Simple/Detailed list rendering, so the same cred looks the same in every view.
                {
                    ImVec2 rMin = ImGui::GetItemRectMin();
                    ImVec2 rMax = ImGui::GetItemRectMax();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    float iconY = rMin.y + 2.0f;
                    float iconX = rMax.x - 12.0f;
                    if (cols.pin_fav && it.is_favorite)
                    {
                        ImVec2 sz = ImGui::CalcTextSize(ICON_MDI_HEART);
                        iconX -= sz.x;
                        dl->AddText(ImVec2(iconX, iconY), GetFavoriteColor(), ICON_MDI_HEART);
                        iconX -= 3.0f;
                    }
                    if (cols.pin_fav && it.is_pinned)
                    {
                        ImVec2 sz = ImGui::CalcTextSize(ICON_MDI_PIN);
                        iconX -= sz.x;
                        dl->AddText(ImVec2(iconX, iconY), ImGui::GetColorU32(colors::MainColor), ICON_MDI_PIN);
                        iconX -= 3.0f;
                    }

                    // Security badges on second line (right-aligned)
                    float badgeY = rMin.y + ImGui::GetTextLineHeight() + 2.0f;
                    float badgeX = rMax.x - 12.0f;
                    if (s.sec_highlight_weak && s.sec_weak_ids.count(it.id))
                    {
                        ImVec2 sz = ImGui::CalcTextSize(ICON_MDI_ALERT);
                        badgeX -= sz.x;
                        dl->AddText(ImVec2(badgeX, badgeY), IM_COL32(174, 41, 41, 255), ICON_MDI_ALERT);
                        badgeX -= 3.0f;
                    }
                    if (s.sec_highlight_reused && s.sec_reused_ids.count(it.id))
                    {
                        ImVec2 sz = ImGui::CalcTextSize(ICON_MDI_REPEAT);
                        badgeX -= sz.x;
                        dl->AddText(ImVec2(badgeX, badgeY), IM_COL32(190, 156, 63, 255), ICON_MDI_REPEAT);
                        badgeX -= 3.0f;
                    }
                    if (s.sec_highlight_exposed && s.sec_exposed_ids.count(it.id))
                    {
                        ImVec2 sz = ImGui::CalcTextSize(ICON_MDI_EARTH);
                        badgeX -= sz.x;
                        dl->AddText(ImVec2(badgeX, badgeY), IM_COL32(142, 68, 173, 255), ICON_MDI_EARTH);
                        badgeX -= 3.0f;
                    }
                    if (s.sec_highlight_aging && s.sec_aging_ids.count(it.id))
                    {
                        ImVec2 sz = ImGui::CalcTextSize(ICON_MDI_CLOCK_ALERT);
                        badgeX -= sz.x;
                        dl->AddText(ImVec2(badgeX, badgeY), IM_COL32(39, 174, 157, 255), ICON_MDI_CLOCK_ALERT);
                        badgeX -= 3.0f;
                    }
                }
                // Row divider with shadow lip
                {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    float x0 = p.x;
                    float x1 = x0 + fullW - 12.0f;
                    float y = p.y - 2.0f;
                    bool dark = IsDarkTheme();
                    dl->AddLine(ImVec2(x0, y), ImVec2(x1, y),
                        dark ? IM_COL32(0, 0, 0, 30) : IM_COL32(0, 0, 0, 18), 1.0f);
                    dl->AddLine(ImVec2(x0, y + 1.0f), ImVec2(x1, y + 1.0f),
                        dark ? IM_COL32(255, 255, 255, 8) : IM_COL32(255, 255, 255, 120), 1.0f);
                }

                ImGui::PopID();
            }
            ImGui::Unindent(8.0f);
        }

        float mlScrollY = ImGui::GetScrollY();
        ImVec2 mlScrollMin = ImGui::GetWindowPos();
        float mlScrollW = ImGui::GetWindowSize().x;
        ImGui::EndChild(); // ##3p_list_scroll
        // DrawScrollTopFade(mlScrollMin, mlScrollW, mlScrollY, -3.0f);

        ImGui::EndChild(); // ##3p_list
    }

    // Hint line for security card context
    static void DrawHintLine(const char* icon, const char* text, ImU32 iconCol)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, iconCol);
        ImGui::TextUnformatted(icon);
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 4);
        ImGui::TextDisabled("%s", text);
    }

    // Security analysis card for password credentials (no toggle, always shown).
    // Reused-password sibling navigation is handled inline (see header decl).
    static void DrawSecurityCard(const AccordionItem& c, ShellState& s,
        uint32_t activeVaultKey, GetPasswordFn get_password_fn, float contentW)
    {
        if (c.type != CredType::Password) return;

        const float padX = 12.0f;
        const float padY = 8.0f;
        const float pillGap = 12.0f;
        const float rounding = 8.0f;

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // --- Card content ---
        ImVec2 cardStart = ImGui::GetCursorScreenPos();
        float cardW = contentW;

        ImGui::Dummy(ImVec2(0, padY));
        ImGui::Indent(padX);

        // Strength meter
        const char* pw = get_password_fn ? get_password_fn(c.id) : "";
        std::string pwStr = pw ? pw : "";
        float meterW = cardW - padX * 2;
        DrawStrengthMeterCompact(pwStr, meterW);
        ImGui::Spacing();

        // Strength context hints
        if (!pwStr.empty())
        {
            helpers::PwStrength ps = helpers::analyze_password(pwStr);
            ImGui::PushFont(render::FontSmall);
            const ImU32 goodCol = IM_COL32(80, 180, 90, 200);
            const ImU32 warnCol = IM_COL32(240, 140, 60, 200);

            int classes = (int)ps.hasLower + (int)ps.hasUpper + (int)ps.hasDigit + (int)ps.hasSymbol;
            int hints = 0;

            if (ps.shortPwd && hints < 3)
            { DrawHintLine(ICON_MDI_CLOSE, "Too short (< 8 chars)", warnCol); hints++; }
            if (ps.allSame && hints < 3)
            { DrawHintLine(ICON_MDI_CLOSE, "All same character", warnCol); hints++; }
            if (ps.simpleSeq && hints < 3)
            { DrawHintLine(ICON_MDI_CLOSE, "Simple sequence detected", warnCol); hints++; }
            if (classes < 2 && hints < 3)
            { DrawHintLine(ICON_MDI_CLOSE, "Only one character type", warnCol); hints++; }
            if (classes >= 3 && hints < 3)
            { DrawHintLine(ICON_MDI_CHECK, "Mixed character types", goodCol); hints++; }
            if ((int)pwStr.size() >= 16 && hints < 3)
            { DrawHintLine(ICON_MDI_CHECK, "Long password (16+)", goodCol); hints++; }

            ImGui::PopFont();
            if (hints > 0) ImGui::Spacing();
        }

        // Issue pills
        bool isReused  = s.sec_reused_ids.count(c.id) != 0;
        bool isExposed = s.sec_exposed_ids.count(c.id) != 0;
        bool isWeak    = s.sec_weak_ids.count(c.id) != 0;
        bool isAging   = s.sec_aging_ids.count(c.id) != 0;

        if (isReused || isExposed || isWeak || isAging)
        {
            ImGui::Dummy(ImVec2(0, 2));
            float cursorX = ImGui::GetCursorScreenPos().x;
            float cursorY = ImGui::GetCursorScreenPos().y;
            float xOff = 0.0f;

            auto DrawPill = [&](const char* icon, const char* label, ImU32 col) {
                std::string text = std::string(icon) + " " + label;
                ImVec2 sz = ImGui::CalcTextSize(text.c_str());
                dl->AddText(ImVec2(cursorX + xOff, cursorY), col, text.c_str());
                xOff += sz.x + pillGap;
            };

            if (isWeak)
                DrawPill(ICON_MDI_ALERT, "Weak", IM_COL32(174, 41, 41, 255));
            // "Reused" pill is suppressed here because the dedicated "Reused
            // across N other credentials" header below already states the same
            // thing more usefully (with a count + clickable sibling links).
            // Showing both read as a duplicate badge.
            if (isExposed)
                DrawPill(ICON_MDI_EARTH, "Exposed", IM_COL32(142, 68, 173, 255));
            if (isAging)
            {
                int64_t pw_set_at = 0;
                if (!c.password_history.empty() && c.password_history[0].changed_at_ms > 0)
                    pw_set_at = c.password_history[0].changed_at_ms;
                else if (c.created_at_ms > 0)
                    pw_set_at = c.created_at_ms;

                std::string ageLabel = "Old";
                if (pw_set_at > 0)
                {
                    int64_t ageDays = (helpers::now_unix_ms() - pw_set_at) / (86400LL * 1000LL);
                    ageLabel = std::to_string(ageDays) + "d old";
                }
                DrawPill(ICON_MDI_CLOCK_ALERT, ageLabel.c_str(), IM_COL32(39, 174, 157, 255));
            }

            float lineH = ImGui::CalcTextSize("A").y;
            ImGui::Dummy(ImVec2(0, lineH + 2));
        }

        // Reused password detail — flow-wrapped clickable links (matches Windows).
        // Clicking navigates: three-pane mode selects the cred; other modes open
        // its accordion row and scroll to it. This is "navigate to" semantics,
        // NOT "open editor" — the inline card lets you flip through reused
        // siblings without leaving the list.
        if (isReused && !pwStr.empty())
        {
            const auto& creds = GetActiveVaultCreds();
            struct ReusedEntry { int id; std::string title; };
            std::vector<ReusedEntry> reused;
            for (const auto& other : creds) {
                if (other.id == c.id || other.is_deleted() || other.type != CredType::Password) continue;
                if (other.password == pwStr) {
                    reused.push_back({ other.id, other.title.empty() ? "(Untitled)" : other.title });
                }
            }
            if (!reused.empty()) {
                ImGui::PushFont(render::FontSmall);
                char header[64];
                snprintf(header, sizeof(header), ICON_MDI_REPEAT " Reused across %d other credential%s",
                         (int)reused.size(), reused.size() == 1 ? "" : "s");
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(190, 156, 63, 255));  // amber, matches Reused pill
                ImGui::TextUnformatted(header);
                ImGui::PopStyleColor();

                // Link colors: blue idle → cyan/orange on hover (theme-aware)
                const bool dk2 = IsDarkTheme();
                ImU32 linkCol      = dk2 ? IM_COL32(120, 170, 240, 255) : IM_COL32(40, 110, 200, 255);
                ImU32 linkHoverCol = dk2 ? IM_COL32(255, 180, 80, 255)  : IM_COL32(220, 110, 30, 255);

                const float commaW = ImGui::CalcTextSize(", ").x;
                const float lineH  = ImGui::GetTextLineHeight();
                const float regionMinX = ImGui::GetCursorScreenPos().x;
                const float regionMaxX = regionMinX + ImGui::GetContentRegionAvail().x;
                float curX = regionMinX;
                float curY = ImGui::GetCursorScreenPos().y;

                for (size_t i = 0; i < reused.size(); i++) {
                    const auto& entry = reused[i];
                    ImGui::PushID((int)i);

                    ImVec2 textSize = ImGui::CalcTextSize(entry.title.c_str());
                    float entryW = textSize.x + (i + 1 < reused.size() ? commaW : 0.0f);

                    // Wrap to next line if this entry won't fit
                    if (i > 0 && curX + entryW > regionMaxX) {
                        curX = regionMinX;
                        curY += lineH + 2.0f;
                    }

                    ImGui::SetCursorScreenPos(ImVec2(curX, curY));
                    ImGui::InvisibleButton("##reused_link", textSize);
                    bool hovered = ImGui::IsItemHovered();
                    bool clicked = ImGui::IsItemClicked();

                    ImU32 col = hovered ? linkHoverCol : linkCol;
                    ImDrawList* dl_l = ImGui::GetWindowDrawList();
                    dl_l->AddText(ImVec2(curX, curY), col, entry.title.c_str());
                    if (hovered) {
                        dl_l->AddLine(
                            ImVec2(curX, curY + textSize.y),
                            ImVec2(curX + textSize.x, curY + textSize.y), col);
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }

                    if (clicked) {
                        if (s.view_mode == ViewMode::ThreePane) {
                            s.three_pane_selected_id = entry.id;
                        } else {
                            uint64_t targetKey = MakeRowKey(activeVaultKey, entry.id);
                            g_open.insert(targetKey);
                            g_scroll_to_open[targetKey] = 2;
                        }
                    }

                    curX += textSize.x;

                    // Comma separator
                    if (i + 1 < reused.size()) {
                        dl_l->AddText(ImVec2(curX, curY), ImGui::GetColorU32(ImGuiCol_TextDisabled), ", ");
                        curX += commaW;
                    }

                    ImGui::PopID();
                }
                // Advance cursor past the last line of flow layout
                ImGui::SetCursorScreenPos(ImVec2(regionMinX, curY + lineH + 2.0f));
                ImGui::PopFont();
                ImGui::Spacing();
            }
        }

        // Separator between password feedback and sync/backup
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float x0 = p.x;
            float x1 = x0 + meterW;
            bool dk = IsDarkTheme();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(x0, p.y), ImVec2(x1, p.y),
                dk ? IM_COL32(255, 255, 255, 12) : IM_COL32(0, 0, 0, 10), 1.0f);
            ImGui::Dummy(ImVec2(0, 1.0f));
        }

        // Sync / Backup status
        bool showSync   = s.sync_logged_in;
        bool hasBackups = !s.footer_backup_meta.empty();

        if (showSync || hasBackups)
        {
            ImGui::Dummy(ImVec2(0, 2));
            std::string statusLine;

            if (showSync)
                statusLine = ICON_MDI_CLOUD_CHECK " Synced";

            if (hasBackups)
            {
                if (!statusLine.empty()) statusLine += "  \xc2\xb7  ";
                statusLine += ICON_MDI_HISTORY " Backed up";
            }

            ImGui::TextDisabled("%s", statusLine.c_str());
        }

        // Share status (view count, expiry)
        if (g_shell_ptr)
            DrawShareStatusRow(c, *g_shell_ptr);

        ImGui::Unindent(padX);
        ImGui::Dummy(ImVec2(0, padY));

        // Card border
        ImVec2 cardEnd = ImGui::GetCursorScreenPos();
        dl->AddRect(
            ImVec2(cardStart.x, cardStart.y),
            ImVec2(cardStart.x + cardW, cardEnd.y),
            IsDarkTheme() ? IM_COL32(255,255,255,20) : IM_COL32(0,0,0,20), rounding, 0, 1.0f);
    }

    static void RenderThreePaneDetailPane(
        const std::vector<AccordionItem>& items,
        ShellState& s,
        uint32_t activeVaultKey,
        GetPasswordFn get_password_fn,
        bool read_only,
        AccordionListResult& out)
    {
        float detailW = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0,0,0,0));
        ImGui::BeginChild("##3p_detail", ImVec2(detailW, 0), ImGuiChildFlags_Borders);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        if (s.three_pane_selected_id == -1)
        {
            // Centered placeholder with icon
            ImVec2 avail = ImGui::GetContentRegionAvail();
            const char* ico = ICON_MDI_KEY;
            const char* msg = "Select an item";
            ImVec2 icoSz = ImGui::CalcTextSize(ico);
            ImVec2 msgSz = ImGui::CalcTextSize(msg);
            float cx = avail.x * 0.5f;
            float cy = avail.y * 0.4f;
            ImGui::SetCursorPos(ImVec2(cx - icoSz.x * 0.5f, cy - icoSz.y - 4.0f));
            ImGui::TextDisabled("%s", ico);
            ImGui::SetCursorPos(ImVec2(cx - msgSz.x * 0.5f, cy + 4.0f));
            ImGui::TextDisabled("%s", msg);
            ImGui::EndChild();
            return;
        }

        // Find the selected item
        const AccordionItem* sel = nullptr;
        for (const auto& it : items)
        {
            if (!it.is_header && it.id == s.three_pane_selected_id)
            { sel = &it; break; }
        }
        if (!sel)
        {
            ImGui::TextDisabled("Item not found.");
            ImGui::EndChild();
            return;
        }

        const auto& c = *sel;

        // Layout constants matching existing row helpers
        const float iconBtnSz = 16.0f;
        const float iconColW  = 30.0f;
        const float rowWidth  = ImGui::GetContentRegionAvail().x - 20.0f;
        const float notesWidth  = ImGui::GetContentRegionAvail().x - 6.0f;
        const float valueColW = rowWidth - iconColW - 10.0f;
        const float rowSpacing = 6.0f;

        // Scrollable content area (action buttons pinned at bottom)
        const float bottomBarH = 38.0f;
        ImGui::BeginChild("##3p_detail_scroll", ImVec2(0, -bottomBarH), ImGuiChildFlags_None);

        // --- Header: favicon + title + type subtitle ---
        ImVec2 headerMin;
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float headerH = 48.0f;
            headerMin = ImGui::GetCursorScreenPos();

            // Reserve header space
            ImGui::Dummy(ImVec2(0, headerH));

            float textX = headerMin.x;

            // Favicon or type icon
            auto srv = favicon::Get(c.website);
            const float faviconSz = 28.0f;
            if (srv)
            {
                float iconY = headerMin.y + (headerH - faviconSz) * 0.5f;
                dl->AddImage((ImTextureID)srv,
                    ImVec2(textX, iconY), ImVec2(textX + faviconSz, iconY + faviconSz));
                textX += faviconSz + 10.0f;
            }
            else
            {
                const char* typeIco = CredTypeIcon(c.type);
                ImVec2 icoSz = ImGui::CalcTextSize(typeIco);
                float iconY = headerMin.y + (headerH - icoSz.y) * 0.5f;
                dl->AddText(ImVec2(textX, iconY),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), typeIco);
                textX += icoSz.x + 10.0f;
            }

            // Two-line: title + type label
            float titleFontSz = render::FontBold ? render::FontBold->LegacySize : ImGui::GetTextLineHeight();
            float subFontSz   = render::FontSmall ? render::FontSmall->LegacySize : ImGui::GetTextLineHeight();
            const char* subText = CredTypeLabel(c.type);

            const float gap = 2.0f;
            float blockH = titleFontSz + gap + subFontSz;
            float blockY = headerMin.y + (headerH - blockH) * 0.5f;

            // Clip title to available width
            float maxTitleW = ImGui::GetContentRegionAvail().x - textX + headerMin.x - 10.0f;
            dl->PushClipRect(ImVec2(textX, blockY), ImVec2(textX + maxTitleW, blockY + titleFontSz + 1.0f), true);
            dl->AddText(render::FontBold, titleFontSz,
                ImVec2(textX, blockY), ImGui::GetColorU32(ImGuiCol_Text), DisplayTitle(c.title));
            dl->PopClipRect();

            dl->AddText(render::FontSmall, subFontSz,
                ImVec2(textX, blockY + titleFontSz + gap),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), subText);
        }

        // Pin/Fav moved to action bar

        ImGui::Spacing();
        ImGui::Spacing();

        // --- Type-conditional rows (reuse existing Draw* helpers) ---
        static std::set<int> visiblePasswordsThreePane;
        const bool isNew = !!(c.changed_fields & FCF_IsNew);

        // Shared container constants for all credential types
        const float cGap = 10.0f, cBoxPad = 10.0f;
        const bool dark = IsDarkTheme();
        ImU32 cDivCol = dark ? IM_COL32(255, 255, 255, 12) : IM_COL32(0, 0, 0, 12);
        auto DrawRowDivider = [&](ImDrawList* dl, float w) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float lineY = p.y - 2.0f;
            dl->AddLine(ImVec2(p.x, lineY), ImVec2(p.x + w, lineY), cDivCol, 1.0f);
            ImGui::Dummy(ImVec2(0, 2.0f));
        };

        // Set lifted child colors for all containers
        LiftedChildColorSet liftedColors;
        liftedColors.bg          = dark ? IM_COL32(255, 255, 255, 6)  : IM_COL32(0, 0, 0, 6);
        liftedColors.borderOuter = dark ? IM_COL32(0, 0, 0, 80)      : IM_COL32(0, 0, 0, 35);
        liftedColors.borderInner = dark ? IM_COL32(255, 255, 255, 30) : IM_COL32(255, 255, 255, 180);
        SetLiftedChildColors(liftedColors);

        if (c.type == CredType::Password)
        {
            // --- About container (website + group + timestamps) ---
            {
                ImGui::PushFont(render::FontSmall);
                ImGui::TextDisabled("About");
                ImGui::PopFont();

                if (BeginLiftedChild("##tp_about", ImVec2(0, 0), ImGuiWindowFlags_NoScrollbar))
                {
                    ImGui::Dummy(ImVec2(0, 6.0f));
                    ImGui::Indent(cBoxPad);

                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Website);
                    DrawWebsiteRow("tp_website", c.website, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing);
                    DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Group);
                    if (!c.group.empty())
                    {
                        DrawInfoRow("tp_group", ICON_MDI_FOLDER, c.group, iconColW, valueColW, rowSpacing - 5);
                        DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                    }
                    g_field_accent_active = false;

                    // Timestamps inside About — absolute date format ("Dec 30, 2025"),
                    // mirrors Windows ui_views.cpp:2125.
                    if (c.created_at_ms != 0 || c.updated_at_ms != 0)
                    {
                        std::string ts;
                        if (c.created_at_ms != 0)
                            ts = "Created " + FormatUnixMsDateLabel(c.created_at_ms);
                        if (c.updated_at_ms != 0)
                        {
                            if (!ts.empty()) ts += "  \xc2\xb7  ";
                            ts += "Updated " + FormatUnixMsDateLabel(c.updated_at_ms);
                        }
                        ImGui::PushFont(render::FontSmall);
                        ImGui::TextDisabled("%s", ts.c_str());
                        ImGui::PopFont();
                        ImGui::Dummy(ImVec2(0, 4.0f));
                    }

                    ImGui::Unindent(cBoxPad);
                }
                EndLiftedChild();
            }

            ImGui::Dummy(ImVec2(0, cGap));

            // --- Account container (user + email + password + TOTP) ---
            // Security container folded in here per the latest detail layout.
            {
                ImGui::PushFont(render::FontSmall);
                ImGui::TextDisabled("Account");
                ImGui::PopFont();

                if (BeginLiftedChild("##tp_account", ImVec2(0, 0), ImGuiWindowFlags_NoScrollbar))
                {
                    ImGui::Dummy(ImVec2(0, 6.0f));
                    ImGui::Indent(cBoxPad);

                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_User);
                    DrawCopyRow("tp_user", ICON_MDI_ACCOUNT, c.user, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.user));
                    DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Email);
                    DrawEmailRow("tp_email", c.email, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.email));
                    DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Password);
                    DrawPasswordRow("tp_pw", c.id, get_password_fn, c.password_history, visiblePasswordsThreePane, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing);
                    DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_TotpSecret);
                    DrawTOTPRow("tp_totp", c.totp_secret, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing);

                    ImGui::Dummy(ImVec2(0, 1.0f));
                    ImGui::Unindent(cBoxPad);
                }
                EndLiftedChild();
            }
        }
        else if (c.type == CredType::CreditCard)
        {
            // --- Card Details container ---
            {
                ImGui::PushFont(render::FontSmall);
                ImGui::TextDisabled("Card Details");
                ImGui::PopFont();

                if (BeginLiftedChild("##tp_card_details", ImVec2(0, 0), ImGuiWindowFlags_NoScrollbar))
                {
                    ImGui::Dummy(ImVec2(0, 6.0f));
                    ImGui::Indent(cBoxPad);

                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardNumber);
                    DrawMaskedCopyRow("tp_card", ICON_MDI_CREDIT_CARD, c.card_number, 4, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing);
                    DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardExpiry);
                    DrawCopyRow("tp_expiry", ICON_MDI_CALENDAR, c.card_expiry, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing);
                    DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardCvv);
                    DrawMaskedCopyRow("tp_cvv", ICON_MDI_LOCK, c.card_cvv, 0, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing);
                    if (!c.card_address.empty()) {
                        DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardAddress);
                        DrawCopyRow("tp_card_addr", ICON_MDI_MAP_MARKER, c.card_address, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.card_address));
                    }
                    DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardBrand);
                    DrawInfoRow("tp_brand", ICON_MDI_TAG, c.card_brand, iconColW, valueColW, rowSpacing);
                    if (!c.card_city.empty()) {
                        DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardCity);
                        DrawInfoRow("tp_card_city", ICON_MDI_CITY, c.card_city, iconColW, valueColW, rowSpacing);
                    }
                    if (!c.card_postal_code.empty()) {
                        DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_CardPostalCode);
                        DrawInfoRow("tp_card_postal", ICON_MDI_MAILBOX, c.card_postal_code, iconColW, valueColW, rowSpacing);
                    }

                    ImGui::Dummy(ImVec2(0, 4.0f));
                    ImGui::Unindent(cBoxPad);
                }
                EndLiftedChild();
            }

            ImGui::Dummy(ImVec2(0, cGap));

            // --- Account container (cardholder) ---
            {
                ImGui::PushFont(render::FontSmall);
                ImGui::TextDisabled("Account");
                ImGui::PopFont();

                if (BeginLiftedChild("##tp_card_account", ImVec2(0, 0), ImGuiWindowFlags_NoScrollbar))
                {
                    ImGui::Dummy(ImVec2(0, 6.0f));
                    ImGui::Indent(cBoxPad);

                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Cardholder);
                    DrawCopyRow("tp_holder", ICON_MDI_ACCOUNT, c.cardholder_name, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.cardholder_name));
                    if (!c.address.empty())
                    {
                        DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Address);
                        DrawCopyRow("tp_addr", ICON_MDI_MAP_MARKER, c.address, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.address));
                    }

                    ImGui::Dummy(ImVec2(0, 4.0f));
                    ImGui::Unindent(cBoxPad);
                }
                EndLiftedChild();
            }

            ImGui::Dummy(ImVec2(0, cGap));

            // --- About container (website + group + timestamps) ---
            {
                ImGui::PushFont(render::FontSmall);
                ImGui::TextDisabled("About");
                ImGui::PopFont();

                if (BeginLiftedChild("##tp_card_about", ImVec2(0, 0), ImGuiWindowFlags_NoScrollbar))
                {
                    ImGui::Dummy(ImVec2(0, 6.0f));
                    ImGui::Indent(cBoxPad);

                    g_field_accent_active = isNew || !!(c.changed_fields & FCF_Website);
                    DrawInfoRow("tp_website", ICON_MDI_EARTH, c.website, iconColW, valueColW, rowSpacing);
                    if (!c.group.empty())
                    {
                        DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                        g_field_accent_active = isNew || !!(c.changed_fields & FCF_Group);
                        DrawInfoRow("tp_group", ICON_MDI_FOLDER, c.group, iconColW, valueColW, rowSpacing - 5);
                    }
                    g_field_accent_active = false;

                    if (c.created_at_ms != 0 || c.updated_at_ms != 0)
                    {
                        DrawRowDivider(ImGui::GetWindowDrawList(), rowWidth);
                        std::string ts;
                        if (c.created_at_ms != 0) ts += "Created " + FormatUnixMsDateLabel(c.created_at_ms);
                        if (c.updated_at_ms != 0) {
                            if (!ts.empty()) ts += "  \xc2\xb7  ";
                            ts += "Updated " + FormatUnixMsDateLabel(c.updated_at_ms);
                        }
                        ImGui::TextDisabled("%s", ts.c_str());
                    }

                    ImGui::Dummy(ImVec2(0, 4.0f));
                    ImGui::Unindent(cBoxPad);
                }
                EndLiftedChild();
            }
        }
        else if (c.type == CredType::Identity)
        {
            g_field_accent_active = isNew || !!(c.changed_fields & FCF_FullName);
            DrawCopyRow("tp_name", ICON_MDI_ACCOUNT, c.full_name, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.full_name));
            g_field_accent_active = isNew || !!(c.changed_fields & FCF_IdType);
            DrawInfoRow("tp_idtype", ICON_MDI_CARD_ACCOUNT_DETAILS, c.id_type, iconColW, valueColW, rowSpacing);
            g_field_accent_active = isNew || !!(c.changed_fields & FCF_IdNumber);
            DrawCopyRow("tp_idnum", ICON_MDI_POUND, c.id_number, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.id_number));
            g_field_accent_active = isNew || !!(c.changed_fields & FCF_DateOfBirth);
            DrawInfoRow("tp_dob", ICON_MDI_CAKE_VARIANT, c.date_of_birth, iconColW, valueColW, rowSpacing);
            g_field_accent_active = isNew || !!(c.changed_fields & FCF_ExpiryDate);
            DrawInfoRow("tp_expiry", ICON_MDI_CALENDAR, c.expiry_date, iconColW, valueColW, rowSpacing);
            g_field_accent_active = isNew || !!(c.changed_fields & FCF_Country);
            DrawInfoRow("tp_country", ICON_MDI_EARTH, c.country, iconColW, valueColW, rowSpacing);
            g_field_accent_active = isNew || !!(c.changed_fields & FCF_Address);
            DrawCopyRow("tp_addr", ICON_MDI_MAP_MARKER, c.address, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.address));
            g_field_accent_active = isNew || !!(c.changed_fields & FCF_Phone);
            DrawCopyRow("tp_phone", ICON_MDI_PHONE, c.phone, iconColW, valueColW, rowWidth, iconBtnSz, rowSpacing, MaskIfPrivate(c.phone));
        }
        else if (c.type == CredType::SecureNote)
        {
            g_field_accent_active = false;
            if (!c.notes.empty())
            {
                const uint64_t rowKey = MakeRowKey(activeVaultKey, c.id);

                // Check deferred reprompt approval
                if (g_notes_reprompt_pending == rowKey && IsRepromptApproved(RepromptAction::RevealNotes))
                {
                    g_notes_visible.insert(rowKey);
                    ClearRepromptApproval(RepromptAction::RevealNotes);
                    g_notes_reprompt_pending = 0;
                }
                bool notesRevealed = g_notes_visible.count(rowKey) != 0;

                if (notesRevealed)
                {
                    // "Hide notes" link with chevron
                    {
                        const char* linkLabel = "Hide notes";
                        const char* chevron = " " ICON_MDI_CHEVRON_UP;
                        ImVec2 textSz = ImGui::CalcTextSize(linkLabel);
                        ImVec2 chevSz = ImGui::CalcTextSize(chevron);
                        ImVec2 totalSz(textSz.x + chevSz.x, textSz.y);

                        ImVec2 pos = ImGui::GetCursorScreenPos();
                        ImGui::InvisibleButton("##tp_securenote_hide", totalSz);
                        bool clicked = ImGui::IsItemClicked();
                        bool hovered = ImGui::IsItemHovered();

                        ImU32 col = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                        ImDrawList* dl2 = ImGui::GetWindowDrawList();
                        dl2->AddText(pos, col, linkLabel);
                        dl2->AddText(ImVec2(pos.x + textSz.x, pos.y), col, chevron);

                        if (hovered)
                        {
                            float y = pos.y + textSz.y + 1.0f;
                            dl2->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + textSz.x, y), col, 1.0f);
                        }

                        if (clicked)
                            g_notes_visible.erase(rowKey);
                    }
                    ImGui::Spacing();
                    ImGui::Spacing();

                    // Read-only notes display
                    std::string notesDisplay = c.notes;
                    float notesInlineW = ImGui::GetContentRegionAvail().x;
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, IsDarkTheme() ? IM_COL32(255,255,255,4) : IM_COL32(0,0,0,4));
                    ImGui::PushStyleColor(ImGuiCol_Border,  IsDarkTheme() ? IM_COL32(255,255,255,20) : IM_COL32(0,0,0,20));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 10.0f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
                    // Wrapped read-only notes display
                    {
                        ImVec2 fpad = ImGui::GetStyle().FramePadding;
                        ImVec2 fpos = ImGui::GetCursorScreenPos();
                        float innerW = notesInlineW - fpad.x * 2;
                        ImVec2 tSz = ImGui::CalcTextSize(notesDisplay.c_str(), nullptr, false, innerW);
                        float frameH = ImMax(tSz.y + fpad.y * 2, 60.0f);
                        ImDrawList* ndl = ImGui::GetWindowDrawList();
                        ndl->AddRectFilled(fpos, ImVec2(fpos.x + notesInlineW, fpos.y + frameH),
                            ImGui::GetColorU32(ImGuiCol_FrameBg), 6.0f);
                        ndl->AddRect(fpos, ImVec2(fpos.x + notesInlineW, fpos.y + frameH),
                            ImGui::GetColorU32(ImGuiCol_Border), 6.0f);
                        ImGui::SetCursorScreenPos(ImVec2(fpos.x + fpad.x, fpos.y + fpad.y));
                        float wrapX = ImGui::GetCursorPosX() + innerW;
                        ImGui::PushTextWrapPos(wrapX);
                        ImGui::TextUnformatted(notesDisplay.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::SetCursorScreenPos(ImVec2(fpos.x, fpos.y + frameH));
                    }
                    ImGui::PopStyleVar(3);
                    ImGui::PopStyleColor(2);
                }
                else
                {
                    // "Reveal notes" link with chevron
                    const char* linkLabel = "Reveal notes";
                    const char* chevron = " " ICON_MDI_CHEVRON_DOWN;
                    ImVec2 textSz = ImGui::CalcTextSize(linkLabel);
                    ImVec2 chevSz = ImGui::CalcTextSize(chevron);
                    ImVec2 totalSz(textSz.x + chevSz.x, textSz.y);

                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##tp_securenote_reveal", totalSz);
                    bool clicked = ImGui::IsItemClicked();
                    bool hovered = ImGui::IsItemHovered();

                    ImU32 col = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                    ImDrawList* dl2 = ImGui::GetWindowDrawList();
                    dl2->AddText(pos, col, linkLabel);
                    dl2->AddText(ImVec2(pos.x + textSz.x, pos.y), col, chevron);

                    if (hovered)
                    {
                        float y = pos.y + textSz.y + 1.0f;
                        dl2->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + textSz.x, y), col, 1.0f);
                    }

                    if (clicked)
                    {
                        g_notes_reprompt_pending = rowKey;
                        if (RequestReprompt(RepromptAction::RevealNotes, ""))
                        {
                            g_notes_visible.insert(rowKey);
                            g_notes_reprompt_pending = 0;
                        }
                    }
                }
                ImGui::Spacing();
            }
        }
        g_field_accent_active = false;

        // Group row (types that don't have their own About container)
        if (c.type != CredType::Password && c.type != CredType::CreditCard && !c.group.empty())
        {
            g_field_accent_active = isNew || !!(c.changed_fields & FCF_Group);
            DrawInfoRow("tp_group", ICON_MDI_FOLDER, c.group, iconColW, valueColW, rowSpacing);
            g_field_accent_active = false;
        }

        // Tabbed section: Security Analysis + Notes (below password/security card)
        {
            bool hasNotes = (c.type != CredType::SecureNote && !c.notes.empty());
            bool hasSecurity = (c.type == CredType::Password);

            if (hasNotes || hasSecurity)
            {
                ImGui::Dummy(ImVec2(0, 10));

                // Track active tab per credential
                static std::unordered_map<int, int> s_detail_tab; // 0 = security, 1 = notes
                int& activeTab = s_detail_tab[c.id];

                // Default to security if available, otherwise notes
                if (!hasSecurity && activeTab == 0) activeTab = 1;

                // Deferred reprompt approval for notes tab switch
                if (g_notes_tab_reprompt_cred_id == c.id && IsRepromptApproved(RepromptAction::RevealNotes))
                {
                    activeTab = 1;
                    ClearRepromptApproval(RepromptAction::RevealNotes);
                    g_notes_tab_reprompt_cred_id = -1;
                }
                if (!hasNotes && activeTab == 1) activeTab = 0;

                // Draw split-pill tab bar
                {
                    const bool dark = IsDarkTheme();
                    const float tabH = 28.0f;
                    const float rounding = 8.0f;
                    const float pad_x = 10.0f;
                    const float divW = 1.0f;
                    const ImU32 mutedCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                    const ImU32 textCol  = ImGui::GetColorU32(ImGuiCol_Text);
                    const ImU32 pillBg   = dark ? IM_COL32(40, 40, 43, 255)  : IM_COL32(235, 235, 238, 255);
                    const ImU32 pillHov  = dark ? IM_COL32(50, 50, 54, 255)  : IM_COL32(225, 225, 228, 255);
                    const ImU32 lipCol   = dark ? IM_COL32(13, 18, 26, 92)   : IM_COL32(71, 84, 102, 56);
                    const ImU32 divCol   = dark ? IM_COL32(255,255,255, 12)  : IM_COL32(0, 0, 0, 15);
                    ImDrawList* dl = ImGui::GetWindowDrawList();

                    // Build tab entries
                    struct TabEntry { const char* id; const char* icon; const char* label; int idx; };
                    TabEntry tabs[2];
                    int numTabs = 0;
                    if (hasSecurity)
                        tabs[numTabs++] = { "##tab_security", ICON_MDI_SHIELD_CHECK, "Security", 0 };
                    if (hasNotes)
                        tabs[numTabs++] = { "##tab_notes", ICON_MDI_NOTE_TEXT, "Notes", 1 };

                    // Measure regions
                    float regionW[2] = {};
                    for (int i = 0; i < numTabs; i++)
                    {
                        bool active = (activeTab == tabs[i].idx);
                        if (active)
                        {
                            char full[128];
                            snprintf(full, sizeof(full), "%s %s", tabs[i].icon, tabs[i].label);
                            regionW[i] = ImGui::CalcTextSize(full).x + pad_x * 2.0f;
                        }
                        else
                            regionW[i] = ImGui::CalcTextSize(tabs[i].icon).x + pad_x * 2.0f;
                    }

                    float totalW = regionW[0] + (numTabs > 1 ? regionW[1] : 0.0f);
                    ImVec2 pillPos = ImGui::GetCursorScreenPos();

                    // Lip shadow behind pill
                    const float lipOff = 1.5f;
                    dl->AddRectFilled(
                        ImVec2(pillPos.x, pillPos.y + lipOff),
                        ImVec2(pillPos.x + totalW, pillPos.y + tabH + lipOff),
                        lipCol, rounding);

                    // Pill background
                    dl->AddRectFilled(
                        pillPos,
                        ImVec2(pillPos.x + totalW, pillPos.y + tabH),
                        pillBg, rounding);

                    // Draw each region
                    float curX = pillPos.x;
                    for (int i = 0; i < numTabs; i++)
                    {
                        bool active = (activeTab == tabs[i].idx);
                        float w = regionW[i];
                        ImVec2 rMin(curX, pillPos.y);
                        ImVec2 rMax(curX + w, pillPos.y + tabH);

                        // Hit test
                        ImGui::SetCursorScreenPos(rMin);
                        ImGui::InvisibleButton(tabs[i].id, ImVec2(w, tabH));
                        bool hov = ImGui::IsItemHovered();
                        if (ImGui::IsItemClicked())
                        {
                            if (tabs[i].idx == 1 && cfg::get_reprompt_reveal_notes())
                            {
                                g_notes_tab_reprompt_cred_id = c.id;
                                RequestReprompt(RepromptAction::RevealNotes, "");
                            }
                            else
                                activeTab = tabs[i].idx;
                        }

                        // Hover highlight
                        {
                            ImDrawFlags rndFlags = 0;
                            if (i == 0) rndFlags = ImDrawFlags_RoundCornersLeft;
                            else if (i == numTabs - 1) rndFlags = ImDrawFlags_RoundCornersRight;
                            if (!active)
                            {
                                ImU32 inactiveFill = dark ? IM_COL32(0, 0, 0, 40) : IM_COL32(0, 0, 0, 18);
                                dl->AddRectFilled(rMin, rMax, inactiveFill, rounding, rndFlags);
                            }
                            if (hov && !active)
                                dl->AddRectFilled(rMin, rMax, pillHov, rounding, rndFlags);
                        }

                        // Text
                        if (active)
                        {
                            char full[128];
                            snprintf(full, sizeof(full), "%s %s", tabs[i].icon, tabs[i].label);
                            ImVec2 sz = ImGui::CalcTextSize(full);
                            dl->AddText(ImVec2(curX + pad_x, pillPos.y + (tabH - sz.y) * 0.5f), textCol, full);
                        }
                        else
                        {
                            ImVec2 sz = ImGui::CalcTextSize(tabs[i].icon);
                            dl->AddText(ImVec2(curX + pad_x, pillPos.y + (tabH - sz.y) * 0.5f),
                                hov ? textCol : mutedCol, tabs[i].icon);
                        }

                        curX += w;

                        // 3D divider between regions
                        if (i < numTabs - 1)
                        {
                            ImU32 shineLine  = dark ? IM_COL32(255, 255, 255, 8) : IM_COL32(255, 255, 255, 80);
                            ImU32 shadowLine = dark ? IM_COL32(0, 0, 0, 60) : IM_COL32(0, 0, 0, 30);
                            // Flip divider based on which side is active
                            ImU32 leftLine  = active ? shineLine  : shadowLine;
                            ImU32 rightLine = active ? shadowLine : shineLine;
                            dl->AddLine(
                                ImVec2(curX, pillPos.y),
                                ImVec2(curX, pillPos.y + tabH + lipOff),
                                leftLine, 1.0f);
                            dl->AddLine(
                                ImVec2(curX + 1.0f, pillPos.y),
                                ImVec2(curX + 1.0f, pillPos.y + tabH + lipOff),
                                rightLine, 1.0f);
                            curX += 2.0f;
                        }
                    }

                    // Advance cursor past the pill
                    ImGui::SetCursorScreenPos(ImVec2(pillPos.x, pillPos.y + tabH + 4.0f));
                }

                // Tab content
                if (activeTab == 0 && hasSecurity)
                {
                    DrawSecurityCard(c, s, activeVaultKey, get_password_fn, ImGui::GetContentRegionAvail().x);
                }
                else if (activeTab == 1 && hasNotes)
                {
                    ImGui::PushFont(render::FontSmall);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, IsDarkTheme() ? IM_COL32(255,255,255,4) : IM_COL32(0,0,0,4));
                    ImGui::PushStyleColor(ImGuiCol_Border,  IsDarkTheme() ? IM_COL32(255,255,255,20) : IM_COL32(0,0,0,20));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 10.0f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
                    {
                        float notesW = ImGui::GetContentRegionAvail().x;
                        ImVec2 fpad = ImGui::GetStyle().FramePadding;
                        float innerW = notesW - fpad.x * 2;
                        ImVec2 tSz = ImGui::CalcTextSize(c.notes.c_str(), nullptr, false, innerW);
                        float frameH = ImMax(tSz.y + fpad.y * 2, 40.0f);
                        ImVec2 fpos = ImGui::GetCursorScreenPos();
                        ImDrawList* ndl = ImGui::GetWindowDrawList();
                        ndl->AddRectFilled(fpos, ImVec2(fpos.x + notesW, fpos.y + frameH),
                            ImGui::GetColorU32(ImGuiCol_FrameBg), 6.0f);
                        ndl->AddRect(fpos, ImVec2(fpos.x + notesW, fpos.y + frameH),
                            ImGui::GetColorU32(ImGuiCol_Border), 6.0f);
                        ImGui::SetCursorScreenPos(ImVec2(fpos.x + fpad.x, fpos.y + fpad.y));
                        float wrapX = ImGui::GetCursorPosX() + innerW;
                        ImGui::PushTextWrapPos(wrapX);
                        ImGui::TextUnformatted(c.notes.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::SetCursorScreenPos(ImVec2(fpos.x, fpos.y + frameH));
                    }
                    ImGui::PopStyleVar(3);
                    ImGui::PopStyleColor(3);
                    ImGui::PopFont();
                }

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }

        // Share status row
        DrawShareStatusRow(c, s);

        // Timestamps for non-Password types (Password has them in About container)
        if (c.type != CredType::Password && c.type != CredType::CreditCard && (c.created_at_ms != 0 || c.updated_at_ms != 0))
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            auto FormatAgo = [](int64_t ms) -> std::string {
                int64_t ago = helpers::now_unix_ms() - ms;
                if (ago < 60000LL)          return "just now";
                if (ago < 3600000LL)        return std::to_string(ago / 60000LL) + "m ago";
                if (ago < 86400000LL)       return std::to_string(ago / 3600000LL) + "h ago";
                if (ago < 604800000LL)      return std::to_string(ago / 86400000LL) + "d ago";
                if (ago < 2592000000LL)     return std::to_string(ago / 604800000LL) + "w ago";
                return FormatUnixMsDateOnly(ms);
            };

            std::string ts;
            if (c.created_at_ms != 0)
                ts = "Created " + FormatAgo(c.created_at_ms);
            if (c.updated_at_ms != 0)
            {
                if (!ts.empty()) ts += "  \xc2\xb7  ";
                ts += "Updated " + FormatAgo(c.updated_at_ms);
            }
            ImGui::TextDisabled("%s", ts.c_str());
        }

        float dtScrollY = ImGui::GetScrollY();
        ImVec2 dtScrollMin = ImGui::GetWindowPos();
        float dtScrollW = ImGui::GetWindowSize().x;
        ImGui::EndChild(); // ##3p_detail_scroll
        // DrawScrollTopFade(dtScrollMin, dtScrollW, dtScrollY, -2.0f);

        // --- Sticky action bar ---
        //ImGui::Separator();
        ImGui::Spacing();
        {
            const float ibSz = 28.0f;
            const float btnH = 26.0f;
            const float delW = 95.0f;

            if (!read_only)
            {
                if (IconSquareBtn("##ab_pin", c.is_pinned ? ICON_MDI_PIN : ICON_MDI_PIN, c.is_pinned ? "Unpin" : "Pin", ibSz, c.is_pinned, false, ImGui::GetColorU32(colors::MainColor)))
                    out.toggle_pin_id = c.id;
                ImGui::SameLine(0, 4);
                if (IconSquareBtn("##ab_fav", c.is_favorite ? ICON_MDI_HEART : ICON_MDI_HEART, c.is_favorite ? "Unfavorite" : "Favorite", ibSz, c.is_favorite, false, IM_COL32(220, 60, 60, 255)))
                    out.toggle_fav_id = c.id;
                ImGui::SameLine(0, 4);
            }

#ifndef PD_OFFLINE_BUILD
            if (IconSquareBtn("##ab_share", ICON_MDI_SHARE_VARIANT, "Share", ibSz))
                out.anon_share_id = c.id;
#endif

            if (!read_only)
            {
                ImGui::SameLine(0, 4);
                if (IconSquareBtn("##ab_edit", ICON_MDI_PENCIL, "Edit", ibSz))
                    out.edit_open_id = c.id;

                // Delete — far right
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - delW);
                if (HoldToActionButton("##tp_delete", ICON_MDI_DELETE " Delete", 1.0f, ImVec2(delW, btnH), 4.0f))
                    out.delete_id = c.id;
            }
        }

        ImGui::EndChild(); // ##3p_detail
    }

    static AccordionListResult RenderThreePaneView(
        const std::vector<AccordionItem>& items,
        uint32_t activeVaultKey,
        GetPasswordFn get_password_fn,
        bool read_only)
    {
        AccordionListResult out{};
        auto& s = *g_shell_ptr;
        const float dt = ImGui::GetIO().DeltaTime;

        // --- Auto-collapse: compute effective collapsed state ---
        const bool sb_base_collapsed = s.three_pane_sidebar_collapsed;
        const bool ml_base_collapsed = s.three_pane_list_collapsed;

        const bool sb_effective = (s.three_pane_sidebar_auto_collapse && sb_base_collapsed)
            ? !g_3p_sidebar_hover_expanded : sb_base_collapsed;
        const bool ml_effective = (s.three_pane_list_auto_collapse && ml_base_collapsed)
            ? !g_3p_list_hover_expanded : ml_base_collapsed;

        s.three_pane_sidebar_collapsed = sb_effective;
        s.three_pane_list_collapsed    = ml_effective;

        // --- Animate pane widths ---
        const float sbTargetW = sb_effective ? 50.0f : 170.0f;
        const float mlTargetW = ml_effective ? 72.0f : 280.0f;

        if (g_3p_sidebar_anim_w < 0) g_3p_sidebar_anim_w = sbTargetW;
        if (g_3p_list_anim_w    < 0) g_3p_list_anim_w    = mlTargetW;

        float step   = k3pAnimSpeed * dt;
        float stepML = step * 1.4f;  // middle pane collapses slightly quicker
        if (g_3p_sidebar_anim_w < sbTargetW)      g_3p_sidebar_anim_w = ImMin(g_3p_sidebar_anim_w + step, sbTargetW);
        else if (g_3p_sidebar_anim_w > sbTargetW) g_3p_sidebar_anim_w = ImMax(g_3p_sidebar_anim_w - step, sbTargetW);
        if (g_3p_list_anim_w < mlTargetW)          g_3p_list_anim_w = ImMin(g_3p_list_anim_w + stepML, mlTargetW);
        else if (g_3p_list_anim_w > mlTargetW)     g_3p_list_anim_w = ImMax(g_3p_list_anim_w - stepML, mlTargetW);

        // --- Render three columns ---
        RenderThreePaneSidebar(s, g_3p_sidebar_anim_w, items);
        ImVec2 sbMin = ImGui::GetItemRectMin();
        ImVec2 sbMax = ImGui::GetItemRectMax();

        ImGui::SameLine(0, 0);
        RenderThreePaneMiddleList(items, s, activeVaultKey, g_3p_list_anim_w);
        ImVec2 mlMin = ImGui::GetItemRectMin();
        ImVec2 mlMax = ImGui::GetItemRectMax();

        ImGui::SameLine(0, 0);
        RenderThreePaneDetailPane(items, s, activeVaultKey, get_password_fn, read_only, out);

        // Vertical divider lines between panes
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 lineCol = ImGui::GetColorU32(ImGuiCol_Separator, 0.4f);

            // Between middle list and detail pane
            dl->AddLine(ImVec2(mlMax.x, mlMin.y), ImVec2(mlMax.x, mlMax.y), lineCol, 1.0f);
        }

        // Vertical shadow on left edge of detail pane
        {
            ImVec2 dMin = ImGui::GetItemRectMin();
            ImVec2 dMax = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float shadowW = 10.0f;
            ImU32 dark  = IM_COL32(0, 0, 0, 35);
            ImU32 clear = IM_COL32(0, 0, 0, 0);
            // Left edge shadow
            dl->AddRectFilledMultiColor(
                ImVec2(dMin.x - shadowW, dMin.y),
                ImVec2(dMin.x, dMax.y),
                clear, dark, dark, clear);

            // Bottom edge shadow (rendered inside, fading upward)
            dl->AddRectFilledMultiColor(
                ImVec2(dMin.x, dMax.y - shadowW),
                ImVec2(dMax.x, dMax.y),
                clear, clear, dark, dark);
        }


        // --- Restore base collapsed state (only if toggle wasn't clicked) ---
        if (s.three_pane_sidebar_collapsed == sb_effective)
            s.three_pane_sidebar_collapsed = sb_base_collapsed;
        if (s.three_pane_list_collapsed == ml_effective)
            s.three_pane_list_collapsed = ml_base_collapsed;

        // --- Hover detection for auto-collapse ---
        const bool anyPopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        // Sidebar hover
        if (s.three_pane_sidebar_auto_collapse && s.three_pane_sidebar_collapsed)
        {
            bool hovered = (mouse.x >= sbMin.x && mouse.x <= sbMax.x &&
                            mouse.y >= sbMin.y && mouse.y <= sbMax.y);
            if (hovered)
            {
                g_3p_sidebar_hover_expanded = true;
                g_3p_sidebar_leave_timer = 0.0f;
            }
            else if (g_3p_sidebar_hover_expanded && !anyPopupOpen)
            {
                g_3p_sidebar_leave_timer += dt;
                if (g_3p_sidebar_leave_timer >= k3pCollapseDelay)
                {
                    g_3p_sidebar_hover_expanded = false;
                    g_3p_sidebar_leave_timer = 0.0f;
                }
            }
        }
        else
        {
            g_3p_sidebar_hover_expanded = false;
            g_3p_sidebar_leave_timer = 0.0f;
        }

        // Middle list hover
        if (s.three_pane_list_auto_collapse && s.three_pane_list_collapsed)
        {
            bool hovered = (mouse.x >= mlMin.x && mouse.x <= mlMax.x &&
                            mouse.y >= mlMin.y && mouse.y <= mlMax.y);
            if (hovered)
            {
                g_3p_list_hover_expanded = true;
                g_3p_list_leave_timer = 0.0f;
            }
            else if (g_3p_list_hover_expanded && !anyPopupOpen)
            {
                g_3p_list_leave_timer += dt;
                if (g_3p_list_leave_timer >= k3pCollapseDelay)
                {
                    g_3p_list_hover_expanded = false;
                    g_3p_list_leave_timer = 0.0f;
                }
            }
        }
        else
        {
            g_3p_list_hover_expanded = false;
            g_3p_list_leave_timer = 0.0f;
        }

        // Validate selection still exists in current filtered list
        if (s.three_pane_selected_id != -1)
        {
            bool found = false;
            for (const auto& it : items)
                if (!it.is_header && it.id == s.three_pane_selected_id) { found = true; break; }
            if (!found) s.three_pane_selected_id = -1;
        }

        return out;
    }

    // ============================================================
    // TABLE VIEW
    // ============================================================

    static AccordionListResult RenderTableView(
        const std::vector<AccordionItem>& items,
        uint32_t activeVaultKey,
        GetPasswordFn get_password_fn,
        bool read_only)
    {
        AccordionListResult out{};
        auto& s = *g_shell_ptr;

        // Build flat index of non-header items
        static std::vector<int> sortedIdx;
        static int              lastItemCount = -1;
        bool needsSort = false;

        // Rebuild index when items change (include headers for grouped display)
        {
            int totalCount = (int)items.size();

            bool rebuild = false;
            if (totalCount != lastItemCount)
                rebuild = true;
            else if (!sortedIdx.empty() && sortedIdx.back() >= totalCount)
                rebuild = true;

            if (rebuild)
            {
                sortedIdx.clear();
                for (int i = 0; i < totalCount; i++)
                    sortedIdx.push_back(i);
                lastItemCount = totalCount;
                needsSort = true;
            }
        }

        if (sortedIdx.empty())
        {
            ImGui::TextDisabled("No items match this view.");
            return out;
        }

        // Column IDs
        enum TableColID {
            Col_Checkbox = 0, Col_Index, Col_Type, Col_Title, Col_Username,
            Col_Email, Col_Website, Col_Group, Col_Date, Col_PinFav
        };

        // Detect single-type filter for dynamic columns
        enum TableTypeFilter { TF_Mixed = 0, TF_Passwords, TF_Cards, TF_Identity, TF_Notes };
        TableTypeFilter typeFilter = TF_Mixed;
        {
            bool hasP = s.selected_groups.count("@Passwords") > 0;
            bool hasC = s.selected_groups.count("@Cards") > 0;
            bool hasI = s.selected_groups.count("@Identity") > 0;
            bool hasN = s.selected_groups.count("@Notes") > 0;
            int count = (int)hasP + (int)hasC + (int)hasI + (int)hasN;
            if (count == 1)
            {
                if (hasP) typeFilter = TF_Passwords;
                else if (hasC) typeFilter = TF_Cards;
                else if (hasI) typeFilter = TF_Identity;
                else if (hasN) typeFilter = TF_Notes;
            }
        }

        // Dynamic column headers for cols 3-6
        const char* col3Hdr = "Username";
        const char* col4Hdr = "Email";
        const char* col5Hdr = "Website";
        const char* col6Hdr = "Group";
        switch (typeFilter)
        {
        case TF_Cards:    col3Hdr = "Cardholder"; col4Hdr = "Card Number"; col5Hdr = "Expiry"; col6Hdr = "Brand"; break;
        case TF_Identity: col3Hdr = "Full Name";  col4Hdr = "ID Number";   col5Hdr = "Phone";  col6Hdr = "Country"; break;
        case TF_Notes:    col3Hdr = "Notes";      col4Hdr = "";            col5Hdr = "";       col6Hdr = ""; break;
        default: break;
        }

        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp;

        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float avail_h = ImGui::GetContentRegionAvail().y;
        const bool dark = IsDarkTheme();

        // Soften table visuals
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, IM_COL32(0, 0, 0, 0));   // hide row dividers (we draw our own)
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, IM_COL32(0, 0, 0, 0));  // hide outer border
        ImGui::PushStyleColor(ImGuiCol_TableRowBg,    dark ? IM_COL32(0, 0, 0, 0) : IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, dark ? IM_COL32(255, 255, 255, 6) : IM_COL32(0, 0, 0, 8));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));  // right-click popup padding
        if (!ImGui::BeginTable("##cred_table", 9, tableFlags, ImVec2(avail_w, avail_h)))
        {
            ImGui::PopStyleVar();  // WindowPadding
            ImGui::PopStyleColor(4);
            return out;
        }

        // Column setup (cols 3-6 are dynamic based on type filter)
        ImGui::TableSetupColumn("###cb", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoReorder | ImGuiTableColumnFlags_NoHeaderLabel, 28.0f, Col_Checkbox);
        ImGui::TableSetupColumn("Icon",     ImGuiTableColumnFlags_WidthFixed, 30.0f, Col_Type);
        ImGui::TableSetupColumn("Title",    ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 3.0f, Col_Title);
        ImGui::TableSetupColumn(col3Hdr, ImGuiTableColumnFlags_WidthStretch, 2.0f, Col_Username);
        ImGui::TableSetupColumn(col4Hdr, ImGuiTableColumnFlags_WidthStretch | (typeFilter == TF_Notes ? ImGuiTableColumnFlags_Disabled : 0), 2.5f, Col_Email);
        ImGui::TableSetupColumn(col5Hdr, ImGuiTableColumnFlags_WidthStretch | (typeFilter == TF_Notes ? ImGuiTableColumnFlags_Disabled : 0), 2.0f, Col_Website);
        ImGui::TableSetupColumn(col6Hdr, ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide | (typeFilter == TF_Notes ? ImGuiTableColumnFlags_Disabled : 0), 1.5f, Col_Group);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 90.0f, Col_Date);
        ImGui::TableSetupColumn("#",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoResize, 36.0f, Col_Index);
        ImGui::TableSetupScrollFreeze(0, 1);
        // Custom header row with select-all checkbox (compact)
        // Minimal header row — we draw text manually for tight height control
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, dark ? IM_COL32(255, 255, 255, 6) : IM_COL32(0, 0, 0, 4));
        ImGui::TableHeadersRow();  // register headers for sorting (required by ImGui)
        ImGui::PopStyleColor();

        // Draw custom header overlay on top
        {
            ImDrawList* hdl = ImGui::GetWindowDrawList();
            // Bottom separator
            float hdrY = ImGui::GetCursorScreenPos().y;
            float hdrX0 = ImGui::GetWindowPos().x;
            float hdrX1 = hdrX0 + ImGui::GetWindowWidth();
            float lineY = IM_ROUND(hdrY + 6.0f);
            hdl->AddLine(ImVec2(hdrX0, lineY), ImVec2(hdrX1, lineY),
                dark ? IM_COL32(0, 0, 0, 60) : IM_COL32(0, 0, 0, 20), 1.0f);
        }

        // Column header right-click popup (blocks credential row popup underneath)
        {
            ImGuiTable* tbl = ImGui::GetCurrentTable();
            float hdrMinY = tbl->OuterRect.Min.y;
            float hdrMaxY = tbl->InnerClipRect.Min.y;
            float hdrMinX = tbl->OuterRect.Min.x;
            float hdrMaxX = tbl->OuterRect.Max.x;
            ImVec2 mpos = ImGui::GetMousePos();
            bool hdrHovered = (mpos.y >= hdrMinY && mpos.y < hdrMaxY &&
                               mpos.x >= hdrMinX && mpos.x < hdrMaxX &&
                               ImGui::IsWindowHovered(ImGuiHoveredFlags_None));
            if (hdrHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("##tbl_col_hdr_popup");

            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,  8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 8));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(4, 4));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(6, 2));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, IsDarkTheme() ? IM_COL32(26, 26, 28, 255) : IM_COL32(250, 250, 252, 255));
            ImGui::PushStyleColor(ImGuiCol_Border,  IsDarkTheme() ? IM_COL32(255,255,255,25) : IM_COL32(0,0,0,30));
            if (ImGui::BeginPopup("##tbl_col_hdr_popup"))
            {
                PopupStyleBegin();
                ImGui::PushFont(render::FontSmall);
                for (int ci = 0; ci < tbl->ColumnsCount; ci++)
                {
                    ImGuiTableColumn& col = tbl->Columns[ci];
                    if (col.Flags & ImGuiTableColumnFlags_NoHide) continue;
                    const char* colName = ImGui::TableGetColumnName(ci);
                    if (!colName || !colName[0]) continue;
                    bool enabled = (col.Flags & ImGuiTableColumnFlags_Disabled) == 0 && col.IsEnabled;
                    if (ImGui::MenuItem(colName, NULL, enabled))
                        ImGui::TableSetColumnEnabled(ci, !enabled);
                }
                ImGui::PopFont();
                PopupStyleEnd();
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(5);
        }


        // Sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsDirty || needsSort)
            {
                // Always preserve original order with group headers.
                // Sort data items within each group.
                sortedIdx.clear();
                for (int i = 0; i < (int)items.size(); i++)
                    sortedIdx.push_back(i);

                if (sortSpecs->SpecsCount > 0)
                {
                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                    const bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);

                    // Find group boundaries and sort within each group
                    int groupStart = 0;
                    for (int i = 0; i <= (int)sortedIdx.size(); i++)
                    {
                        bool atEnd = (i == (int)sortedIdx.size());
                        bool isHdr = !atEnd && items[sortedIdx[i]].is_header;

                        if (atEnd || isHdr)
                        {
                            // Sort the data range [groupStart, i)
                            if (i > groupStart)
                            {
                                std::sort(sortedIdx.begin() + groupStart, sortedIdx.begin() + i,
                                    [&](int a, int b) -> bool
                                    {
                                        const AccordionItem& ia = items[a];
                                        const AccordionItem& ib = items[b];
                                        if (ia.is_pinned != ib.is_pinned) return ia.is_pinned;
                                        if (ia.is_favorite != ib.is_favorite) return ia.is_favorite;
                                        int cmp = 0;
                                        switch (spec.ColumnUserID)
                                        {
                                        case Col_Type:     cmp = (int)ia.type - (int)ib.type; break;
                                        case Col_Title:    cmp = strcasecmp(ia.title.c_str(), ib.title.c_str()); break;
                                        case Col_Username: cmp = strcasecmp(ia.user.c_str(), ib.user.c_str()); break;
                                        case Col_Email:    cmp = strcasecmp(ia.email.c_str(), ib.email.c_str()); break;
                                        case Col_Website:  cmp = strcasecmp(ia.website.c_str(), ib.website.c_str()); break;
                                        case Col_Group:    cmp = strcasecmp(ia.group.c_str(), ib.group.c_str()); break;
                                        case Col_Date:
                                            if (ia.updated_at_ms < ib.updated_at_ms) cmp = -1;
                                            else if (ia.updated_at_ms > ib.updated_at_ms) cmp = 1;
                                            break;
                                        default: break;
                                        }
                                        return asc ? (cmp < 0) : (cmp > 0);
                                    });
                            }
                            groupStart = i + 1; // skip the header
                        }
                    }
                }
                sortSpecs->SpecsDirty = false;
            }
        }

        // Deferred popup state
        static int s_table_popup_idx = -1;

        // Security highlight colors
        const ImU32 weakTint    = IM_COL32(174, 41, 41, 30);
        const ImU32 reusedTint  = IM_COL32(190, 156, 63, 30);
        const ImU32 exposedTint = IM_COL32(142, 68, 173, 20);
        const ImU32 agingTint   = IM_COL32(39, 174, 157, 20);
        const ImU32 selTint    = IM_COL32(
            (int)(colors::SecondColor.x * 255),
            (int)(colors::SecondColor.y * 255),
            (int)(colors::SecondColor.z * 255),
            38); // ~15% alpha
        const ImU32 hoverTint  = IsDarkTheme() ? IM_COL32(255, 255, 255, 15) : IM_COL32(0, 0, 0, 15);

        const ImVec2 mp = ImGui::GetMousePos();

        // Taller data rows
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 10));

        // Clipper for virtualized rows (extra padding only when group headers are present)
        // Also precompute credential-only index (excluding header rows) for # column
        bool hasGroupHeaders = false;
        int headerCountSoFar = 0;
        std::vector<int> credIndex(sortedIdx.size());
        for (int i = 0; i < (int)sortedIdx.size(); i++)
        {
            if (items[sortedIdx[i]].is_header)
            {
                hasGroupHeaders = true;
                headerCountSoFar++;
                credIndex[i] = 0; // unused for headers
            }
            else
                credIndex[i] = i - headerCountSoFar + 1;
        }
        int paddingRows = 0;
        if (hasGroupHeaders)
        {
            switch (s.group_mode)
            {
                case ui::GroupMode::Alphabetical:    paddingRows = 4; break;
                case ui::GroupMode::MonthCreated:
                case ui::GroupMode::MonthUpdated:     paddingRows = 5; break;
                case ui::GroupMode::Group:            paddingRows = 5; break;
                case ui::GroupMode::PinnedFavorites:  paddingRows = 5; break;
                default:                             paddingRows = 0; break;
            }
        }
        const int totalRows = (int)sortedIdx.size() + paddingRows;
        ImGuiListClipper clipper;
        clipper.Begin(totalRows);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
            {
                if (row >= (int)sortedIdx.size())
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Dummy(ImVec2(0, 0));
                    continue;
                }

                const AccordionItem& c = items[sortedIdx[row]];

                // Group header row
                if (c.is_header)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Dummy(ImVec2(0, 2.0f));
                    if (ImGui::TableSetColumnIndex(2))
                    {
                        ImGui::PushFont(render::FontSmall);
                        ImGui::TextDisabled("%s", c.header_label.c_str());
                        ImGui::PopFont();
                    }
                    continue;
                }

                const uint64_t rowKey = MakeRowKey(activeVaultKey, c.id);
                const bool selected = IsSelected(rowKey);

                ImGui::TableNextRow();

                // Capture row Y for hover detection
                ImGui::TableSetColumnIndex(0);
                float rowStartY = ImGui::GetCursorScreenPos().y;

                // Row background tints (security highlights only — selected/hover handled below)
                if (s.sec_highlight_weak && s.sec_weak_ids.count(c.id))
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, weakTint);
                else if (s.sec_highlight_reused && s.sec_reused_ids.count(c.id))
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, reusedTint);
                else if (s.sec_highlight_exposed && s.sec_exposed_ids.count(c.id))
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, exposedTint);
                else if (s.sec_highlight_aging && s.sec_aging_ids.count(c.id))
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, agingTint);
                else if (c.is_changed)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                        (c.changed_fields & FCF_IsNew) ? kNewRowTint : kChangedRowTint);

                ImGui::PushID((int)rowKey);

                // Col 0: Checkbox
                {
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    float offsetY = (ImGui::GetTextLineHeightWithSpacing() - 15.0f) * 0.5f;
                    ImGui::SetCursorScreenPos(ImVec2(pos.x + 4.0f, pos.y + offsetY));
                    bool cbVal = selected;
                    if (ImGui::Checkbox2("", &cbVal))
                        ToggleSelected(rowKey);
                }

                // Col 1: Favicon / type icon (rounded, no background)
                if (ImGui::TableSetColumnIndex(1))
                {
                    auto srv = favicon::Get(c.website);
                    if (srv)
                    {
                        const float icoSz = 22.0f;
                        ImVec2 pos = ImGui::GetCursorScreenPos();
                        float offsetY = (ImGui::GetTextLineHeightWithSpacing() - icoSz) * 0.5f;
                        ImGui::GetWindowDrawList()->AddImageRounded(
                            (ImTextureID)srv,
                            ImVec2(pos.x, pos.y + offsetY),
                            ImVec2(pos.x + icoSz, pos.y + offsetY + icoSz),
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32(255, 255, 255, 255), 4.0f);
                        ImGui::Dummy(ImVec2(icoSz, icoSz));
                    }
                    else
                        ImGui::TextUnformatted(CredTypeIcon(c.type));
                    if (ImGui::IsItemHovered())
                        SetTooltipPadded("%s", CredTypeLabel(c.type));
                }

                // Col 2: Title + Pin/Fav icons (right-aligned with bg mask)
                if (ImGui::TableSetColumnIndex(2))
                {
                    float colW = ImGui::GetContentRegionAvail().x;
                    ImVec2 cellPos = ImGui::GetCursorScreenPos();
                    float lineH = ImGui::GetTextLineHeight();

                    // Measure icons width
                    float iconsW = 0.0f;
                    if (c.is_pinned)   iconsW += ImGui::CalcTextSize(ICON_MDI_PIN).x + 2.0f;
                    if (c.is_favorite) iconsW += ImGui::CalcTextSize(ICON_MDI_HEART).x + 2.0f;

                    // Title text (clipped to leave room for icons)
                    float titleW = iconsW > 0 ? colW - iconsW - 4.0f : colW;
                    ImGui::PushFont(render::FontBold);
                    TextEllipsisClipped(DisplayTitle(c.title), titleW);
                    ImGui::PopFont();

                    // Draw pin/fav icons right-aligned (no mask needed — title already clipped)
                    if (iconsW > 0)
                    {
                        ImDrawList* tdl = ImGui::GetWindowDrawList();
                        float iconX = cellPos.x + colW - iconsW;
                        float iconY = cellPos.y;
                        if (c.is_pinned)
                        {
                            tdl->AddText(ImVec2(iconX, iconY), ImGui::GetColorU32(colors::MainColor), ICON_MDI_PIN);
                            iconX += ImGui::CalcTextSize(ICON_MDI_PIN).x + 2.0f;
                        }
                        if (c.is_favorite)
                        {
                            tdl->AddText(ImVec2(iconX, iconY), GetFavoriteColor(), ICON_MDI_HEART);
                        }
                    }
                }

                // Cols 3-6: type-aware content
                if (typeFilter == TF_Cards)
                {
                    if (ImGui::TableSetColumnIndex(3))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        const char* d = MaskIfPrivate(c.cardholder_name) ? MaskIfPrivate(c.cardholder_name) : c.cardholder_name.c_str();
                        TextEllipsisClipped(d, colW);
                    }
                    if (ImGui::TableSetColumnIndex(4))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        std::string masked = c.card_number.size() >= 4
                            ? std::string("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 ") + c.card_number.substr(c.card_number.size() - 4)
                            : c.card_number;
                        TextEllipsisClipped(masked.c_str(), colW);
                    }
                    if (ImGui::TableSetColumnIndex(5))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        TextEllipsisClipped(c.card_expiry.c_str(), colW);
                    }
                    if (ImGui::TableSetColumnIndex(6))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        TextEllipsisClipped(c.card_brand.c_str(), colW);
                    }
                }
                else if (typeFilter == TF_Identity)
                {
                    if (ImGui::TableSetColumnIndex(3))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        const char* d = MaskIfPrivate(c.full_name) ? MaskIfPrivate(c.full_name) : c.full_name.c_str();
                        TextEllipsisClipped(d, colW);
                    }
                    if (ImGui::TableSetColumnIndex(4))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        const char* d = MaskIfPrivate(c.id_number) ? MaskIfPrivate(c.id_number) : c.id_number.c_str();
                        TextEllipsisClipped(d, colW);
                    }
                    if (ImGui::TableSetColumnIndex(5))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        const char* d = MaskIfPrivate(c.phone) ? MaskIfPrivate(c.phone) : c.phone.c_str();
                        TextEllipsisClipped(d, colW);
                    }
                    if (ImGui::TableSetColumnIndex(6))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        TextEllipsisClipped(c.country.c_str(), colW);
                    }
                }
                else if (typeFilter == TF_Notes)
                {
                    if (ImGui::TableSetColumnIndex(3))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        std::string preview = c.notes.substr(0, 80);
                        TextEllipsisClipped(preview.c_str(), colW);
                    }
                }
                else
                {
                    // Mixed / Passwords — default columns
                    if (ImGui::TableSetColumnIndex(3))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        const char* userDisp = MaskIfPrivate(c.user) ? MaskIfPrivate(c.user) : c.user.c_str();
                        TextEllipsisClipped(userDisp, colW);
                    }
                    if (ImGui::TableSetColumnIndex(4))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        const char* emailDisp = MaskIfPrivate(c.email) ? MaskIfPrivate(c.email) : c.email.c_str();
                        TextEllipsisClipped(emailDisp, colW);
                    }
                    if (ImGui::TableSetColumnIndex(5))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        const char* webDisp = c.website.c_str();
                        if (strncmp(webDisp, "https://", 8) == 0) webDisp += 8;
                        else if (strncmp(webDisp, "http://", 7) == 0) webDisp += 7;
                        TextEllipsisClipped(webDisp, colW);
                    }
                    if (ImGui::TableSetColumnIndex(6))
                    {
                        float colW = ImGui::GetContentRegionAvail().x;
                        TextEllipsisClipped(c.group.c_str(), colW);
                    }
                }

                // Col 7: Modified date
                if (ImGui::TableSetColumnIndex(7))
                {
                    std::string dateStr = FormatUnixMsDateOnly(c.updated_at_ms);
                    ImGui::TextUnformatted(dateStr.c_str());
                }

                // Col 8: Index number (credential-only, excludes group headers)
                if (ImGui::TableSetColumnIndex(8))
                {
                    ImGui::TextDisabled("%d", credIndex[row]);
                }

                ImGui::PopID();

                // Row hover + interactions (after all columns rendered)
                // Use table cell bg rect for accurate row bounds
                ImRect cellRect = ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), 0);
                float rowStartYActual = cellRect.Min.y;
                float rowEndY = cellRect.Max.y;
                rowStartY = rowStartYActual;  // update for hover detection
                float rowMinX = ImGui::GetWindowPos().x;
                float rowMaxX = rowMinX + ImGui::GetWindowWidth();
                bool rowHovered = (mp.y >= rowStartY && mp.y < rowEndY &&
                                   mp.x >= rowMinX && mp.x < rowMaxX &&
                                   ImGui::IsWindowHovered(ImGuiHoveredFlags_None));

                // Custom row rendering: shine/shadow depth
                {
                    ImDrawList* rdl = ImGui::GetWindowDrawList();

                    if (selected)
                    {
                        // Selected: strong fill + top shine + bottom shadow
                        ImU32 fillCol = dark ? IM_COL32(255, 255, 255, 25) : IM_COL32(0, 0, 0, 15);
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, fillCol);
                        rdl->AddLine(ImVec2(rowMinX, rowStartY), ImVec2(rowMaxX, rowStartY),
                            dark ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 255), 1.0f);
                        rdl->AddLine(ImVec2(rowMinX, rowEndY - 1.0f), ImVec2(rowMaxX, rowEndY - 1.0f),
                            dark ? IM_COL32(0, 0, 0, 60) : IM_COL32(0, 0, 0, 35), 1.0f);
                    }
                    else if (rowHovered)
                    {
                        // Hovered: subtle fill + shine + shadow
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, hoverTint);
                        rdl->AddLine(ImVec2(rowMinX, rowStartY), ImVec2(rowMaxX, rowStartY),
                            dark ? IM_COL32(255, 255, 255, 14) : IM_COL32(255, 255, 255, 220), 1.0f);
                        rdl->AddLine(ImVec2(rowMinX, rowEndY - 1.0f), ImVec2(rowMaxX, rowEndY - 1.0f),
                            dark ? IM_COL32(0, 0, 0, 35) : IM_COL32(0, 0, 0, 20), 1.0f);
                    }
                    else
                    {
                        // Normal: top highlight + bottom shadow for carved look
                        rdl->AddLine(ImVec2(rowMinX, rowStartY), ImVec2(rowMaxX, rowStartY),
                            dark ? IM_COL32(255, 255, 255, 8) : IM_COL32(255, 255, 255, 160), 1.0f);
                        rdl->AddLine(ImVec2(rowMinX, rowEndY - 1.0f), ImVec2(rowMaxX, rowEndY - 1.0f),
                            dark ? IM_COL32(0, 0, 0, 20) : IM_COL32(0, 0, 0, 14), 1.0f);
                    }
                }

                // Double-click to edit
                if (rowHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    out.edit_open_id = c.id;

                // Left-click row to select and open popup
                if (rowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                    !ImGui::IsPopupOpen("##tbl_col_hdr_popup"))
                {
                    s_table_popup_idx = row;
                    ImGui::OpenPopup("##tbl_row_popup");
                }
            }
        }

        // Deferred right-click popup (outside clipper, inside table)
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,  8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8, 6));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, IsDarkTheme() ? IM_COL32(26, 26, 28, 255) : IM_COL32(250, 250, 252, 255));
        ImGui::PushStyleColor(ImGuiCol_Border,  IsDarkTheme() ? IM_COL32(255,255,255,25) : IM_COL32(0,0,0,30));
        if (s_table_popup_idx >= 0 && s_table_popup_idx < (int)sortedIdx.size() &&
            !items[sortedIdx[s_table_popup_idx]].is_header &&
            ImGui::BeginPopup("##tbl_row_popup"))
        {
            PopupStyleBegin();
            ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
            const AccordionItem& c = items[sortedIdx[s_table_popup_idx]];
            const uint64_t rowKey = MakeRowKey(activeVaultKey, c.id);

            // Per-cred tab state (Details / Notes / Security)
            static std::unordered_map<int, int> s_tbl_popup_tab;
            const bool hasSecurityTab = (c.type == CredType::Password);
            int& tbTab = s_tbl_popup_tab[c.id];

            DrawPopupTabs(c.id, hasSecurityTab, tbTab);

            // Security tab needs a minimum width to keep hint lines from wrapping.
            if (tbTab == 2 && hasSecurityTab) {
                float secW = ImGui::CalcTextSize(ICON_MDI_CLOSE " Contains dictionary word").x + 16.0f;
                ImGui::Dummy(ImVec2(secW, 0));
            }

            // ====== TAB 0: Details (existing rich copy menu) ======
            if (tbTab == 0)
            {
                ImGui::PushFont(render::FontBold);
                ImGui::TextUnformatted(DisplayTitle(c.title));
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 4));

                // Quick-action icon bar: Pin / Fav / Open Website / Open Inbox.
                // Always visible, mirrors the tile popup's Details tab.
                {
                    const float ibSz = 28.0f;
                    const float ibGap = 4.0f;
                    bool hasWebsiteIco = !c.website.empty();
                    std::string webmailIco = GetWebmailUrl(c.email);

                    if (IconSquareBtn("##ctx_pin", c.is_pinned ? ICON_MDI_PIN : ICON_MDI_PIN,
                        c.is_pinned ? "Unpin" : "Pin", ibSz, c.is_pinned, false, ImGui::GetColorU32(colors::MainColor)))
                    { out.toggle_pin_id = c.id; }
                    ImGui::SameLine(0, ibGap);
                    if (IconSquareBtn("##ctx_fav", c.is_favorite ? ICON_MDI_HEART : ICON_MDI_HEART,
                        c.is_favorite ? "Unfavorite" : "Favorite", ibSz, c.is_favorite, false, IM_COL32(220, 60, 60, 255)))
                    { out.toggle_fav_id = c.id; }
                    ImGui::SameLine(0, ibGap);
                    if (!hasWebsiteIco) ImGui::BeginDisabled();
                    if (IconSquareBtn("##ctx_launch", ICON_MDI_EARTH, "Open Website", ibSz))
                    {
                        if (hasWebsiteIco)
                            system((std::string("open \"") + EnsureUrlProtocol(c.website) + "\"").c_str());
                    }
                    if (!hasWebsiteIco) ImGui::EndDisabled();
                    ImGui::SameLine(0, ibGap);
                    if (webmailIco.empty()) ImGui::BeginDisabled();
                    if (IconSquareBtn("##ctx_inbox", ICON_MDI_EMAIL, "Open Inbox", ibSz))
                    {
                        if (!webmailIco.empty())
                            system((std::string("open \"") + webmailIco + "\"").c_str());
                    }
                    if (webmailIco.empty()) ImGui::EndDisabled();
                }

                ImGui::Dummy(ImVec2(0, 2));
                ImGui::Separator();

                // Copied-feedback timers
                static std::unordered_map<std::string, float> s_copied_timers;
                const float copiedDuration = 1.5f;
                float now = (float)ImGui::GetTime();
                auto showCopied = [&](const char* key) -> bool {
                    auto it = s_copied_timers.find(key);
                    return it != s_copied_timers.end() && (now - it->second) < copiedDuration;
                };
                auto markCopied = [&](const char* key) {
                    s_copied_timers[key] = now;
                };

                bool hasUser = !c.user.empty();
                if (BarMenuItem(showCopied("user") ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_ACCOUNT "   Copy Username", false, hasUser))
                { ImGui::SetClipboardText(c.user.c_str()); markCopied("user"); }

                bool hasEmail = !c.email.empty();
                if (BarMenuItem(showCopied("email") ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_EMAIL "   Copy Email", false, hasEmail))
                { ImGui::SetClipboardText(c.email.c_str()); markCopied("email"); }

                bool hasPw = (get_password_fn != nullptr);
                const char* pw = hasPw ? get_password_fn(c.id) : nullptr;
                hasPw = (pw && pw[0] != '\0');
                if (BarMenuItem(showCopied("pw") ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_KEY "   Copy Password", false, hasPw))
                { ClipboardCopyPassword(pw); markCopied("pw"); }

                bool hasTotp = !c.totp_secret.empty();
                if (hasTotp) {
                    auto totpBytes = totp::base32_decode(c.totp_secret);
                    hasTotp = (totpBytes.size() >= 10);
                    if (BarMenuItem(showCopied("totp") ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_CLOCK "   Copy TOTP", false, hasTotp))
                    {
                        std::string code = totp::generate_code_now(totpBytes);
                        ImGui::SetClipboardText(code.c_str());
                        markCopied("totp");
                    }
                } else {
                    BarMenuItem(ICON_MDI_CLOCK "   Copy TOTP", false, false);
                }

                if (c.type == CredType::CreditCard) {
                    ImGui::Separator();
                    bool hasCard = !c.card_number.empty();
                    if (BarMenuItem(showCopied("card") ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_CREDIT_CARD "   Copy Card Number", false, hasCard))
                    { ImGui::SetClipboardText(c.card_number.c_str()); markCopied("card"); }
                    bool hasExpiry = !c.card_expiry.empty();
                    if (BarMenuItem(showCopied("expiry") ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_CALENDAR "   Copy Expiry", false, hasExpiry))
                    { ImGui::SetClipboardText(c.card_expiry.c_str()); markCopied("expiry"); }
                    bool hasCvv = !c.card_cvv.empty();
                    if (BarMenuItem(showCopied("cvv") ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_LOCK "   Copy CVV", false, hasCvv))
                    { ImGui::SetClipboardText(c.card_cvv.c_str()); markCopied("cvv"); }
                    bool hasHolder = !c.cardholder_name.empty();
                    if (BarMenuItem(showCopied("holder") ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_ACCOUNT "   Copy Cardholder", false, hasHolder))
                    { ImGui::SetClipboardText(c.cardholder_name.c_str()); markCopied("holder"); }
                }

                if (c.type == CredType::Identity) {
                    ImGui::Separator();
                    bool hasName = !c.full_name.empty();
                    if (BarMenuItem(showCopied("name") ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_ACCOUNT "   Copy Full Name", false, hasName))
                    { ImGui::SetClipboardText(c.full_name.c_str()); markCopied("name"); }
                    bool hasIdNum = !c.id_number.empty();
                    if (BarMenuItem(showCopied("idnum") ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_CARD_ACCOUNT_DETAILS "   Copy ID Number", false, hasIdNum))
                    { ImGui::SetClipboardText(c.id_number.c_str()); markCopied("idnum"); }
                    bool hasPhone = !c.phone.empty();
                    if (BarMenuItem(showCopied("phone") ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_PHONE "   Copy Phone", false, hasPhone))
                    { ImGui::SetClipboardText(c.phone.c_str()); markCopied("phone"); }
                    bool hasAddr = !c.address.empty();
                    if (BarMenuItem(showCopied("addr") ? ICON_MDI_CHECK "   Copied!" : ICON_MDI_MAP_MARKER "   Copy Address", false, hasAddr))
                    { ImGui::SetClipboardText(c.address.c_str()); markCopied("addr"); }
                }

                ImGui::Separator();

#ifndef PD_OFFLINE_BUILD
                if (BarMenuItem(ICON_MDI_SHARE_VARIANT "   Share Credential", false))
                { out.anon_share_id = c.id; }
#endif

                if (BarMenuItem(ICON_MDI_PENCIL "   Edit Credential", false, !read_only))
                { out.edit_open_id = c.id; }

                ImGui::Separator();

                if (BarMenuItem(ICON_MDI_DELETE "   Delete Credential", false, !read_only))
                { out.delete_id = c.id; }
                if (ImGui::IsItemHovered()) {
                    ImVec2 itemMin = ImGui::GetItemRectMin();
                    ImVec2 itemMax = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddRectFilled(itemMin, itemMax, IM_COL32(180, 40, 40, 50), 4.0f);
                }
            }
            // ====== TAB 1: Notes ======
            else if (tbTab == 1)
            {
                RenderNotesTabContent(c, rowKey);
            }
            // ====== TAB 2: Security ======
            else if (tbTab == 2 && hasSecurityTab)
            {
                int jumpToId = RenderSecurityTabContent(c, get_password_fn);
                if (jumpToId >= 0) {
                    // Table popup writes directly to AccordionListResult.
                    out.edit_open_id = jumpToId;
                }
            }

            ImGui::PopItemFlag();
            PopupStyleEnd();
            ImGui::EndPopup();
        }
        else
        {
            // Popup closed — reset
            s_table_popup_idx = -1;
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(5);

        ImGui::PopStyleVar();  // CellPadding (data rows)
        ImGui::EndTable();
        ImGui::PopStyleColor(4);  // table border/row colors
        ImGui::PopStyleVar();  // WindowPadding (popup)
        return out;
    }

    // ============================================================
    // ACCORDION LIST DISPATCHER
    // ============================================================

    AccordionListResult RenderAccordionList(
        const std::vector<AccordionItem>& items,
        uint32_t activeVaultKey,
        GetPasswordFn get_password_fn,
        bool read_only,
        ViewMode view_mode)
    {
        // Table view — flat spreadsheet
        if (view_mode == ViewMode::Table)
        {
            return RenderTableView(items, activeVaultKey, get_password_fn, read_only);
        }

        // Three-Pane view has its own layout (sidebar + list + detail)
        if (view_mode == ViewMode::ThreePane)
        {
            return RenderThreePaneView(items, activeVaultKey, get_password_fn, read_only);
        }

        AccordionListResult out{};

        if (items.empty())
        {
            ImGui::TextDisabled("No items match this view.");
            return out;
        }

        ImGui::Dummy(ImVec2(0, 6));
        bool hit = false;
        int displayIndex = 1;  // Start at 1 for user-friendly display

        if (view_mode == ViewMode::Tiles)
        {
            const float minTileW = 150.0f;
            const float maxTileW = 200.0f;
            const float tileH = 120.0f;
            const float spacing = 0.0f;

            const float avail = ImGui::GetContentRegionAvail().x -24;
            int columns = std::max(1, (int)((avail + spacing) / (minTileW + spacing)));
            float tileW = (avail - spacing * (columns - 1)) / columns;
            tileW = ImClamp(tileW, minTileW, maxTileW);

            int colIndex = 0;
            bool tileSkipUntilNextHeader = false;
            for (const auto& it : items)
            {
                if (it.is_header)
                {
                    if (colIndex != 0)
                    {
                        ImGui::Dummy(ImVec2(0, spacing));
                        colIndex = 0;
                    }
                    bool hdrCollapsed = g_collapsed_headers.count(it.header_label) > 0;
                    const char* chevron = hdrCollapsed ? ICON_MDI_CHEVRON_RIGHT : ICON_MDI_CHEVRON_DOWN;
                    ImGui::PushID(it.header_label.c_str());
                    ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(0,0,0,0));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  IM_COL32(0,0,0,0));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   IM_COL32(0,0,0,0));
                    float hdrW = ImGui::GetContentRegionAvail().x;
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::Selectable(it.header_label.c_str(), false, 0, ImVec2(hdrW, 0)))
                    {
                        if (hdrCollapsed) g_collapsed_headers.erase(it.header_label);
                        else              g_collapsed_headers.insert(it.header_label);
                        hdrCollapsed = !hdrCollapsed;
                    }
                    ImGui::PopStyleColor();
                    // Draw chevron on far right
                    ImVec2 chevSz = ImGui::CalcTextSize(chevron);
                    ImVec2 rMin = ImGui::GetItemRectMin();
                    ImVec2 rMax = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(rMax.x - chevSz.x - 4.0f, rMin.y + (rMax.y - rMin.y - chevSz.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), chevron);
                    ImGui::PopStyleColor(3);
                    ImGui::PopID();
                    ImGui::Dummy(ImVec2(0, 2));
                    tileSkipUntilNextHeader = hdrCollapsed;
                    continue;
                }

                if (tileSkipUntilNextHeader) continue;

                if (colIndex > 0)
                    ImGui::SameLine(0, spacing);

                ImGui::PushID((void*)(uintptr_t)MakeRowKey(activeVaultKey, it.id));
                AccordionItemAction r = RenderCredentialTile(it, activeVaultKey, get_password_fn, tileW, tileH);
                ImGui::PopID();

                hit |= r.interacted;

                if (!read_only && r.requested_delete)
                    out.delete_id = r.id;

                if (r.edit_open_requested)
                    out.edit_open_id = (r.edit_open_id_override >= 0) ? r.edit_open_id_override : r.id;

#ifndef PD_OFFLINE_BUILD
                if (r.anon_share_requested)
                    out.anon_share_id = r.id;
#endif

                if (r.toggle_pin_requested)
                    out.toggle_pin_id = r.id;

                if (r.toggle_fav_requested)
                    out.toggle_fav_id = r.id;

                colIndex++;
                if (colIndex >= columns)
                {
                    colIndex = 0;
                    ImGui::Dummy(ImVec2(0, spacing));
                }
                displayIndex++;
            }
        }
        else
        {
            bool skipUntilNextHeader = false;
            for (const auto& it : items)
            {
                if (it.is_header)
                {
                    bool hdrCollapsed = g_collapsed_headers.count(it.header_label) > 0;
                    const char* chevron = hdrCollapsed ? ICON_MDI_CHEVRON_RIGHT : ICON_MDI_CHEVRON_DOWN;
                    ImGui::PushID(it.header_label.c_str());
                    ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(0,0,0,0));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  IM_COL32(0,0,0,0));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   IM_COL32(0,0,0,0));
                    float hdrW = ImGui::GetContentRegionAvail().x;
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::Selectable(it.header_label.c_str(), false, 0, ImVec2(hdrW, 0)))
                    {
                        if (hdrCollapsed) g_collapsed_headers.erase(it.header_label);
                        else              g_collapsed_headers.insert(it.header_label);
                        hdrCollapsed = !hdrCollapsed;
                    }
                    ImGui::PopStyleColor();
                    // Draw chevron on far right
                    ImVec2 chevSz = ImGui::CalcTextSize(chevron);
                    ImVec2 rMin = ImGui::GetItemRectMin();
                    ImVec2 rMax = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(rMax.x - chevSz.x - 4.0f, rMin.y + (rMax.y - rMin.y - chevSz.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), chevron);
                    ImGui::PopStyleColor(3);
                    ImGui::PopID();
                    ImGui::Dummy(ImVec2(0, 2));
                    skipUntilNextHeader = hdrCollapsed;
                    continue;
                }

                if (skipUntilNextHeader) continue;

                AccordionItemAction r{};
                if (view_mode == ViewMode::Detailed)
                {
                    r = RenderAccordionItemDetailed(it, activeVaultKey, get_password_fn, read_only, displayIndex++);
                }
                else
                {
                    r = RenderAccordionItem(it, activeVaultKey, get_password_fn, read_only, displayIndex++);
                }

                hit |= r.interacted;

                if (!read_only && r.requested_delete)
                {
                    out.delete_id = r.id;
                    break;
                }

                if (!read_only && r.edit_open_requested)
                {
                    out.edit_open_id = r.id;
                    break;
                }

#ifndef PD_OFFLINE_BUILD
                if (r.anon_share_requested)
                {
                    out.anon_share_id = r.id;
                    break;
                }
#endif

                if (r.toggle_pin_requested)
                {
                    out.toggle_pin_id = r.id;
                    break;
                }

                if (r.toggle_fav_requested)
                {
                    out.toggle_fav_id = r.id;
                    break;
                }

                if (!read_only && r.edit_committed)
                {
                    out.edit_commit_id = r.id;
                    out.edited = r.edited;
                    out.edited_password = r.edited_password;
                    break;
                }

                float gap = g_shell_ptr ? (float)g_shell_ptr->row_gap : 6.0f;
                ImGui::Dummy(ImVec2(0, gap));
            }
        }

        // Click blank space to clear selection:
        // Keep this allowed even in read-only (it doesn't mutate vault data)
        if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        {
            if (!hit && !ImGui::IsAnyItemHovered())
                ClearSelectionForVault(activeVaultKey);
        }

        return out;
    }

    void ForgetRowState(uint32_t activeVaultKey, int id) // NEW signature
    {
        uint64_t rowKey = MakeRowKey(activeVaultKey, id);
        g_selected.erase(MakeRowKey(activeVaultKey, id));
        g_edit.erase(rowKey);
        g_open.erase(rowKey);
        g_tile_state.erase(rowKey);
        g_hover_opened.erase(rowKey);
        g_hover_flipped.erase(rowKey);
        g_notes_visible.erase(rowKey);
        g_card_body_h.erase(rowKey);
        g_card_anim_h.erase(rowKey);
        g_card_closing.erase(rowKey);
    }

    // (Optional convenience)
    void ForgetRowStateRowKey(uint64_t rowKey)
    {
        g_selected.erase(rowKey);
        g_edit.erase(rowKey);
        g_open.erase(rowKey);
        g_tile_state.erase(rowKey);
        g_hover_opened.erase(rowKey);
        g_hover_flipped.erase(rowKey);
        g_notes_visible.erase(rowKey);
        g_card_body_h.erase(rowKey);
        g_card_anim_h.erase(rowKey);
        g_card_closing.erase(rowKey);
    }

    void ResetAllRowState()
    {
        g_edit.clear();
        g_open.clear();
        g_selected.clear();
        g_tile_state.clear();
        g_copy_flash.clear();
        g_hover_opened.clear();
        g_hover_flipped.clear();
        g_notes_visible.clear();
        g_card_body_h.clear();
        g_card_anim_h.clear();
        g_card_closing.clear();
    }

    // g_shell_open, g_sortAnim, g_orderAnim, g_searchAnim defined in ui_controls.cpp

    void ForgetVaultRowState(uint32_t activeVaultKey)
    {
        // g_edit: unordered_map<uint64_t, EditBuffers>
        for (auto it = g_edit.begin(); it != g_edit.end(); )
        {
            uint64_t rk = it->first;
            if ((uint32_t)(rk >> 32) == activeVaultKey)
                it = g_edit.erase(it);
            else
                ++it;
        }

        // g_open: unordered_set<uint64_t>
        for (auto it = g_open.begin(); it != g_open.end(); )
        {
            uint64_t rk = *it;
            if ((uint32_t)(rk >> 32) == activeVaultKey)
                it = g_open.erase(it);
            else
                ++it;
        }

        for (auto it = g_tile_state.begin(); it != g_tile_state.end(); )
        {
            uint64_t rk = it->first;
            if ((uint32_t)(rk >> 32) == activeVaultKey)
                it = g_tile_state.erase(it);
            else
                ++it;
        }

        for (auto it = g_hover_opened.begin(); it != g_hover_opened.end(); )
        {
            uint64_t rk = *it;
            if ((uint32_t)(rk >> 32) == activeVaultKey)
                it = g_hover_opened.erase(it);
            else
                ++it;
        }

        for (auto it = g_hover_flipped.begin(); it != g_hover_flipped.end(); )
        {
            uint64_t rk = *it;
            if ((uint32_t)(rk >> 32) == activeVaultKey)
                it = g_hover_flipped.erase(it);
            else
                ++it;
        }

        for (auto it = g_notes_visible.begin(); it != g_notes_visible.end(); )
        {
            uint64_t rk = *it;
            if ((uint32_t)(rk >> 32) == activeVaultKey)
                it = g_notes_visible.erase(it);
            else
                ++it;
        }

        for (auto it = g_card_body_h.begin(); it != g_card_body_h.end(); )
        {
            uint64_t rk = it->first;
            if ((uint32_t)(rk >> 32) == activeVaultKey)
                it = g_card_body_h.erase(it);
            else
                ++it;
        }

        for (auto it = g_card_anim_h.begin(); it != g_card_anim_h.end(); )
        {
            uint64_t rk = it->first;
            if ((uint32_t)(rk >> 32) == activeVaultKey)
                it = g_card_anim_h.erase(it);
            else
                ++it;
        }

        for (auto it = g_card_closing.begin(); it != g_card_closing.end(); )
        {
            uint64_t rk = *it;
            if ((uint32_t)(rk >> 32) == activeVaultKey)
                it = g_card_closing.erase(it);
            else
                ++it;
        }

        ClearSelectionForVault(activeVaultKey);

    }

    void GetSelectedRowKeys(uint32_t activeVaultKey, std::vector<uint64_t>& outKeys)
    {
        outKeys.clear();
        const uint64_t prefix = (uint64_t(activeVaultKey) << 32);
        for (uint64_t k : g_selected)
            if ((k & 0xFFFFFFFF00000000ull) == prefix)
                outKeys.push_back(k);
    }

} // namespace ui
