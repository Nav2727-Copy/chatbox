#include "gui_app.h"

#include "app_state.h"
#include "chat_client.h"
#include "chat_server.h"
#include "commands.h"
#include "dedicated_server.h"
#include "server_browser.h"
#include "upnp_mapper.h"
#include "utils.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_ask.H>

namespace
{
constexpr double UI_REFRESH_SECONDS = 0.15;
constexpr int CONNECT_WAIT_STEPS = 50;
constexpr int CONNECT_WAIT_MS = 100;

std::string input_value(const Fl_Input& input)
{
    const char* value = input.value();
    return value ? value : "";
}

struct ModalState
{
    bool accepted = false;
    Fl_Window* window = nullptr;
};

void accept_modal_cb(Fl_Widget*, void* data)
{
    auto* state = static_cast<ModalState*>(data);
    state->accepted = true;
    state->window->hide();
}

void cancel_modal_cb(Fl_Widget*, void* data)
{
    auto* state = static_cast<ModalState*>(data);
    state->accepted = false;
    state->window->hide();
}

struct HostSettings
{
    uint16_t port = 0;
    std::string password;
    bool enable_upnp = true;
    bool publish_to_browser = false;
    std::string browser_host;
    std::string room_name;
    std::string public_host;
};

struct JoinSettings
{
    std::string host;
    uint16_t port = 0;
};

std::optional<HostSettings> show_host_dialog()
{
    ModalState state;
    Fl_Double_Window dialog(520, 320, "Host Room");
    state.window = &dialog;
    dialog.set_modal();

    Fl_Int_Input port_input(150, 22, 110, 28, "Port:");
    Fl_Secret_Input password_input(150, 58, 210, 28, "Password:");
    Fl_Check_Button upnp_check(150, 94, 180, 26, "Enable UPnP");
    Fl_Check_Button publish_check(150, 128, 220, 26, "Publish to browser");
    Fl_Input browser_input(150, 164, 250, 28, "Browser host:");
    Fl_Input room_input(150, 200, 250, 28, "Room name:");
    Fl_Input public_input(150, 236, 250, 28, "Public host:");
    Fl_Button ok_button(305, 280, 90, 28, "Host");
    Fl_Button cancel_button(405, 280, 90, 28, "Cancel");

    port_input.value("5000");
    upnp_check.value(1);
    ok_button.callback(accept_modal_cb, &state);
    cancel_button.callback(cancel_modal_cb, &state);
    dialog.end();
    dialog.show();

    while (dialog.shown())
        Fl::wait();

    if (!state.accepted)
        return std::nullopt;

    HostSettings settings;
    if (!parse_port(input_value(port_input), settings.port))
    {
        fl_alert("Enter a valid port number from 0 to 65535.");
        return std::nullopt;
    }

    settings.password = input_value(password_input);
    settings.enable_upnp = upnp_check.value() != 0;
    settings.publish_to_browser = publish_check.value() != 0;
    settings.room_name = input_value(room_input);
    settings.public_host = input_value(public_input);

    if (settings.publish_to_browser &&
        !parse_browser_address(input_value(browser_input), settings.browser_host))
    {
        fl_alert("Enter only the browser server host or IP, without a port.");
        return std::nullopt;
    }

    return settings;
}

std::optional<JoinSettings> show_join_dialog(const char* title, const std::string& initial_host = "",
    uint16_t initial_port = 0)
{
    ModalState state;
    Fl_Double_Window dialog(420, 180, title);
    state.window = &dialog;
    dialog.set_modal();

    Fl_Input host_input(120, 28, 250, 28, "Host:");
    Fl_Int_Input port_input(120, 70, 120, 28, "Port:");
    Fl_Button ok_button(220, 122, 80, 28, "Join");
    Fl_Button cancel_button(310, 122, 80, 28, "Cancel");

    host_input.value(initial_host.c_str());
    if (initial_port != 0)
    {
        std::string port_text = std::to_string(initial_port);
        port_input.value(port_text.c_str());
    }

    ok_button.callback(accept_modal_cb, &state);
    cancel_button.callback(cancel_modal_cb, &state);
    dialog.end();
    dialog.show();

    while (dialog.shown())
        Fl::wait();

    if (!state.accepted)
        return std::nullopt;

    JoinSettings settings;
    settings.host = input_value(host_input);
    if (settings.host.empty())
    {
        fl_alert("Enter a host or IP address.");
        return std::nullopt;
    }

    if (!parse_port(input_value(port_input), settings.port))
    {
        fl_alert("Enter a valid port number from 0 to 65535.");
        return std::nullopt;
    }

    return settings;
}

std::optional<std::string> show_browser_host_dialog()
{
    ModalState state;
    Fl_Double_Window dialog(420, 150, "Browse Servers");
    state.window = &dialog;
    dialog.set_modal();

    Fl_Input browser_input(130, 32, 250, 28, "Browser host:");
    Fl_Button ok_button(220, 92, 80, 28, "Browse");
    Fl_Button cancel_button(310, 92, 80, 28, "Cancel");

    ok_button.callback(accept_modal_cb, &state);
    cancel_button.callback(cancel_modal_cb, &state);
    dialog.end();
    dialog.show();

    while (dialog.shown())
        Fl::wait();

    if (!state.accepted)
        return std::nullopt;

    std::string browser_host;
    if (!parse_browser_address(input_value(browser_input), browser_host))
    {
        fl_alert("Enter only the browser server host or IP, without a port.");
        return std::nullopt;
    }

    return browser_host;
}

std::optional<BrowserEntry> show_browser_choice_dialog(const std::vector<BrowserEntry>& entries)
{
    ModalState state;
    Fl_Double_Window dialog(760, 390, "Published Servers");
    state.window = &dialog;
    dialog.set_modal();

    Fl_Hold_Browser server_list(16, 16, 728, 300);
    Fl_Button join_button(560, 340, 80, 28, "Join");
    Fl_Button cancel_button(650, 340, 80, 28, "Cancel");

    for (const auto& entry : entries)
        server_list.add(browser_entry_summary(entry).c_str());

    join_button.callback(accept_modal_cb, &state);
    cancel_button.callback(cancel_modal_cb, &state);
    dialog.end();
    dialog.show();

    while (dialog.shown())
        Fl::wait();

    if (!state.accepted)
        return std::nullopt;

    const int selected = server_list.value();
    if (selected < 1 || selected > static_cast<int>(entries.size()))
    {
        fl_alert("Choose a published server first.");
        return std::nullopt;
    }

    return entries[static_cast<size_t>(selected - 1)];
}

class GuiApp
{
public:
    int run()
    {
        Fl::scheme("gtk+");
        build_window();
        Fl::add_timeout(UI_REFRESH_SECONDS, refresh_timer_cb, this);
        window_->show();
        const int result = Fl::run();
        Fl::remove_timeout(refresh_timer_cb, this);
        disconnect(false);
        return result;
    }

private:
    void build_window()
    {
        chat_buffer_ = std::make_unique<Fl_Text_Buffer>();
        window_ = std::make_unique<Fl_Double_Window>(980, 640, "chatbox");

        Fl_Box* nick_label = new Fl_Box(14, 14, 72, 28, "Nickname:");
        nick_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        nickname_input_ = new Fl_Input(88, 14, 150, 28);
        nickname_input_->value("guest");

        host_button_ = new Fl_Button(254, 14, 78, 28, "Host");
        join_button_ = new Fl_Button(342, 14, 78, 28, "Join");
        browse_button_ = new Fl_Button(430, 14, 86, 28, "Browse");
        disconnect_button_ = new Fl_Button(526, 14, 104, 28, "Disconnect");

        status_box_ = new Fl_Box(646, 14, 318, 28);
        status_box_->box(FL_DOWN_BOX);
        status_box_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        chat_display_ = new Fl_Text_Display(14, 56, 700, 518);
        chat_display_->buffer(chat_buffer_.get());
        chat_display_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        chat_display_->textfont(FL_COURIER);
        chat_display_->textsize(14);

        users_browser_ = new Fl_Hold_Browser(728, 56, 236, 518, "Users");
        users_browser_->align(FL_ALIGN_TOP_LEFT);

        message_input_ = new Fl_Input(14, 594, 824, 30);
        send_button_ = new Fl_Button(850, 594, 114, 30, "Send");

        host_button_->callback(host_cb, this);
        join_button_->callback(join_cb, this);
        browse_button_->callback(browse_cb, this);
        disconnect_button_->callback(disconnect_cb, this);
        send_button_->callback(send_cb, this);
        message_input_->callback(send_cb, this);
        message_input_->when(FL_WHEN_ENTER_KEY_ALWAYS);
        window_->callback(close_cb, this);
        window_->resizable(chat_display_);
        window_->end();

        set_status("Not connected");
        update_controls();
        refresh_views();
    }

    static void refresh_timer_cb(void* data)
    {
        auto* app = static_cast<GuiApp*>(data);
        app->refresh_views();
        Fl::repeat_timeout(UI_REFRESH_SECONDS, refresh_timer_cb, data);
    }

    static void host_cb(Fl_Widget*, void* data)
    {
        static_cast<GuiApp*>(data)->host_room();
    }

    static void join_cb(Fl_Widget*, void* data)
    {
        auto* app = static_cast<GuiApp*>(data);
        auto settings = show_join_dialog("Join Server");
        if (settings)
            app->join_room(settings->host, settings->port);
    }

    static void browse_cb(Fl_Widget*, void* data)
    {
        static_cast<GuiApp*>(data)->browse_rooms();
    }

    static void disconnect_cb(Fl_Widget*, void* data)
    {
        static_cast<GuiApp*>(data)->disconnect(true);
    }

    static void send_cb(Fl_Widget*, void* data)
    {
        static_cast<GuiApp*>(data)->send_current_message();
    }

    static void close_cb(Fl_Widget*, void* data)
    {
        auto* app = static_cast<GuiApp*>(data);
        app->disconnect(false);
        app->window_->hide();
    }

    void host_room()
    {
        auto settings = show_host_dialog();
        if (!settings)
            return;

        if (!prepare_new_session())
            return;

        try
        {
            server_ = std::make_unique<ChatServer>(
                io_,
                settings->port,
                settings->password,
                nullptr,
                g_nickname);
            server_->load_identities("identities.txt");

            if (!server_->register_local_identity(g_nickname, identity_->public_key_hex))
            {
                server_.reset();
                fl_alert("Your nickname is registered to another identity key.");
                return;
            }

            add_user(g_nickname);
            push_message("[system] Hosting on port " + std::to_string(settings->port));
            push_message("[system] Your identity: " + identity_fingerprint(identity_->public_key_hex));
            if (!settings->password.empty())
                push_message("[system] Room password protection enabled");

            std::string publish_host = sanitize_browser_field(settings->public_host, HOST_MAX_LEN);
            configure_upnp(*settings, publish_host);
            add_lan_address_messages(settings->port, publish_host);
            publish_room(*settings, publish_host);

            hosting_ = true;
            connected_ = true;
            connected_host_.clear();
            connected_port_ = settings->port;
            start_network_thread();
            set_status("Hosting on port " + std::to_string(settings->port));
            update_controls();
            refresh_views();
        }
        catch (const std::exception& ex)
        {
            cleanup_failed_session();
            fl_alert("Could not host the room: %s", ex.what());
        }
        catch (...)
        {
            cleanup_failed_session();
            fl_alert("Could not host the room.");
        }
    }

    void join_room(const std::string& host, uint16_t port)
    {
        if (!prepare_new_session())
            return;

        try
        {
            client_ = std::make_unique<ChatClient>(io_);
            client_->set_identity(*identity_);

            set_status("Connecting to " + host + ":" + std::to_string(port));
            if (!client_->connect(host, port))
            {
                cleanup_failed_session();
                fl_alert("Connection failed.");
                return;
            }

            start_network_thread();

            if (!wait_for([&]
                {
                    return client_->auth_required() || client_->auth_resolved();
                },
                CONNECT_WAIT_STEPS,
                CONNECT_WAIT_MS))
            {
                disconnect(false);
                fl_alert("Server did not respond during authentication.");
                return;
            }

            if (client_->auth_required())
            {
                const char* password = fl_password("Server password:");
                if (!password)
                {
                    disconnect(false);
                    return;
                }

                client_->send("AUTH|" + std::string(password));
                set_status("Authenticating...");
                if (!wait_for([&] { return client_->auth_resolved(); }, CONNECT_WAIT_STEPS, CONNECT_WAIT_MS))
                {
                    disconnect(false);
                    fl_alert("Authentication timed out.");
                    return;
                }

                if (!client_->auth_ok())
                {
                    disconnect(false);
                    fl_alert("Authentication failed. Wrong password.");
                    return;
                }
            }

            client_->identify(g_nickname);
            set_status("Joining...");
            if (!wait_for([&] { return client_->join_resolved(); }, CONNECT_WAIT_STEPS, CONNECT_WAIT_MS))
            {
                disconnect(false);
                fl_alert("Join timed out.");
                return;
            }

            if (client_->was_banned())
            {
                disconnect(false);
                fl_alert("You are banned from this server.");
                return;
            }

            if (client_->nickname_taken())
            {
                disconnect(false);
                fl_alert("Nickname is already in use.");
                return;
            }

            if (client_->join_failed())
            {
                std::string reason = client_->join_failure_reason();
                disconnect(false);
                fl_alert("Join failed: %s", reason.c_str());
                return;
            }

            add_user(g_nickname);
            push_message("[system] Connected to " + host + ":" + std::to_string(port));
            push_message("[system] Your identity: " + identity_fingerprint(identity_->public_key_hex));
            hosting_ = false;
            connected_ = true;
            connected_host_ = host;
            connected_port_ = port;
            set_status("Connected to " + host + ":" + std::to_string(port));
            update_controls();
            refresh_views();
        }
        catch (const std::exception& ex)
        {
            cleanup_failed_session();
            fl_alert("Could not join the room: %s", ex.what());
        }
        catch (...)
        {
            cleanup_failed_session();
            fl_alert("Could not join the room.");
        }
    }

    void browse_rooms()
    {
        auto browser_host = show_browser_host_dialog();
        if (!browser_host)
            return;

        std::vector<BrowserEntry> entries;
        std::string error;
        if (!ServerBrowserClient::list_servers(*browser_host, BROWSER_PORT, entries, error))
        {
            fl_alert("Could not query browser: %s", error.c_str());
            return;
        }

        if (entries.empty())
        {
            fl_alert("No servers are published there right now.");
            return;
        }

        auto selected = show_browser_choice_dialog(entries);
        if (selected)
            join_room(selected->host, selected->port);
    }

    bool prepare_new_session()
    {
        if (connected_ || client_ || server_)
            disconnect(false);

        const std::string nickname = input_value(*nickname_input_);
        if (!is_valid_nickname(nickname))
        {
            fl_alert("Use a 1-31 character nickname without spaces, |, or ,.");
            return false;
        }

        ClientIdentity identity;
        bool identity_created = false;
        if (!load_or_create_identity(nickname, identity, identity_created))
        {
            fl_alert("Could not load or create the local identity key.");
            return false;
        }

        identity_ = identity;
        identity_nickname_ = nickname;
        reset_chat_state();
        g_nickname = nickname;
        io_.restart();

        push_message("[system] " + std::string(identity_created ? "Created" : "Loaded")
            + " local identity " + identity_fingerprint(identity.public_key_hex));
        return true;
    }

    void configure_upnp(const HostSettings& settings, std::string& publish_host)
    {
        if (!settings.enable_upnp)
        {
            push_message("[system] UPnP disabled; forward port "
                + std::to_string(settings.port) + " manually for internet access");
            return;
        }

        upnp_ = std::make_unique<UPnPMapper>();
        push_message("[system] Attempting UPnP port mapping...");

        if (!upnp_->discover())
        {
            push_message("[system] UPnP unavailable: " + upnp_->lastError());
            push_message("[system] Forward port "
                + std::to_string(settings.port) + " manually for internet access");
            return;
        }

        if (!upnp_->openPortBoth(std::to_string(settings.port)))
        {
            push_message("[system] UPnP mapping failed: " + upnp_->lastError());
            push_message("[system] You may need to forward port "
                + std::to_string(settings.port) + " manually");
            return;
        }

        push_message("[system] UPnP OK - port "
            + std::to_string(settings.port) + " opened on router");
        if (!upnp_->externalIP().empty())
        {
            push_message("[system] External address: " + upnp_->externalIP()
                + ":" + std::to_string(settings.port));
            push_message("[system] Share that address with your peer");
            if (publish_host.empty())
                publish_host = upnp_->externalIP();
        }
    }

    void add_lan_address_messages(uint16_t port, std::string& publish_host)
    {
        try
        {
            tcp::resolver resolver(io_);
            auto results = resolver.resolve(boost::asio::ip::host_name(), "");
            push_message("[system] LAN addresses:");
            for (auto& result : results)
            {
                auto addr = result.endpoint().address();
                if (!addr.is_loopback())
                {
                    push_message("[system]   " + addr.to_string() + ":" + std::to_string(port));
                    if (publish_host.empty())
                        publish_host = addr.to_string();
                }
            }
        }
        catch (...) {}
    }

    void publish_room(const HostSettings& settings, std::string publish_host)
    {
        if (!settings.publish_to_browser)
            return;

        if (publish_host.empty())
            publish_host = "127.0.0.1";

        BrowserEntry entry;
        entry.name = settings.room_name.empty()
            ? g_nickname + "'s room"
            : sanitize_browser_field(settings.room_name, BROWSER_NAME_MAX_LEN);
        entry.host = publish_host;
        entry.port = settings.port;
        entry.has_password = !settings.password.empty();

        browser_publisher_ = std::make_unique<ServerBrowserPublisher>(
            settings.browser_host,
            BROWSER_PORT,
            entry,
            [] { return static_cast<int>(connected_users().size()); });

        std::string error;
        if (browser_publisher_->start(error))
        {
            push_message("[system] Published to browser "
                + settings.browser_host + ":" + std::to_string(BROWSER_PORT)
                + " as " + entry.host + ":" + std::to_string(entry.port));
        }
        else
        {
            push_message("[system] Browser publish failed: " + error);
            browser_publisher_.reset();
        }
    }

    void send_current_message()
    {
        if (!connected_)
        {
            fl_alert("Connect to a room first.");
            return;
        }

        const std::string input = input_value(*message_input_);
        if (input.empty())
            return;

        ChatInputContext context;
        context.client = client_.get();
        context.server = server_.get();
        context.local_nickname = g_nickname;
        context.host_commands_enabled = hosting_;

        if (handle_chat_input(input, context) == ChatInputResult::Exit)
            disconnect(true);

        message_input_->value("");
        refresh_views();
    }

    template <typename Predicate>
    bool wait_for(Predicate predicate, int steps, int delay_ms)
    {
        for (int i = 0; i < steps; ++i)
        {
            if (predicate())
                return true;

            Fl::check();
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        return predicate();
    }

    void start_network_thread()
    {
        if (network_thread_.joinable())
            return;

        network_thread_ = std::thread(
            [this]
            {
                try
                {
                    io_.run();
                }
                catch (const std::exception& ex)
                {
                    push_message("[system] Network error: " + std::string(ex.what()));
                }
                catch (...)
                {
                    push_message("[system] Unknown network error");
                }
            });
    }

    void disconnect(bool announce)
    {
        const bool had_session = connected_ || client_ || server_;
        if (client_ && !g_kicked)
        {
            client_->send("LEAVE");
            client_->close();
        }

        if (server_)
        {
            g_shutdown_requested = true;
            server_->stop();
        }

        browser_publisher_.reset();
        upnp_.reset();

        if (network_thread_.joinable())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            io_.stop();
            network_thread_.join();
        }

        client_.reset();
        server_.reset();
        io_.restart();
        connected_ = false;
        hosting_ = false;
        connected_host_.clear();
        connected_port_ = 0;

        if (had_session && announce)
            push_message("[system] Disconnected");

        set_status("Not connected");
        update_controls();
        refresh_views();
    }

    void cleanup_failed_session()
    {
        disconnect(false);
        reset_chat_state();
        refresh_views();
    }

    void refresh_views()
    {
        if (connected_ && client_ && g_kicked)
        {
            disconnect(false);
            set_status("Disconnected: kicked");
        }

        auto messages = message_snapshot();
        std::string rendered;
        for (const auto& message : messages)
        {
            rendered += message;
            rendered += '\n';
        }

        if (rendered != rendered_messages_)
        {
            rendered_messages_ = rendered;
            chat_buffer_->text(rendered_messages_.c_str());
            chat_display_->insert_position(chat_buffer_->length());
            chat_display_->show_insert_position();
        }

        auto users = user_snapshot();
        if (users != rendered_users_)
        {
            rendered_users_ = users;
            users_browser_->clear();
            for (const auto& user : rendered_users_)
                users_browser_->add(user.c_str());
        }
    }

    void set_status(const std::string& status)
    {
        status_text_ = status;
        if (status_box_)
        {
            status_box_->copy_label(status_text_.c_str());
            status_box_->redraw();
        }
    }

    void update_controls()
    {
        if (!host_button_)
            return;

        if (connected_)
        {
            host_button_->deactivate();
            join_button_->deactivate();
            browse_button_->deactivate();
            nickname_input_->deactivate();
            disconnect_button_->activate();
            message_input_->activate();
            send_button_->activate();
        }
        else
        {
            host_button_->activate();
            join_button_->activate();
            browse_button_->activate();
            nickname_input_->activate();
            disconnect_button_->deactivate();
            message_input_->deactivate();
            send_button_->deactivate();
        }
    }

    std::unique_ptr<Fl_Text_Buffer> chat_buffer_;
    std::unique_ptr<Fl_Double_Window> window_;
    Fl_Input* nickname_input_ = nullptr;
    Fl_Button* host_button_ = nullptr;
    Fl_Button* join_button_ = nullptr;
    Fl_Button* browse_button_ = nullptr;
    Fl_Button* disconnect_button_ = nullptr;
    Fl_Box* status_box_ = nullptr;
    Fl_Text_Display* chat_display_ = nullptr;
    Fl_Hold_Browser* users_browser_ = nullptr;
    Fl_Input* message_input_ = nullptr;
    Fl_Button* send_button_ = nullptr;

    boost::asio::io_context io_;
    std::unique_ptr<ChatServer> server_;
    std::unique_ptr<ChatClient> client_;
    std::unique_ptr<UPnPMapper> upnp_;
    std::unique_ptr<ServerBrowserPublisher> browser_publisher_;
    std::optional<ClientIdentity> identity_;
    std::string identity_nickname_;
    std::thread network_thread_;

    bool connected_ = false;
    bool hosting_ = false;
    std::string connected_host_;
    uint16_t connected_port_ = 0;
    std::string status_text_;
    std::string rendered_messages_;
    std::vector<std::string> rendered_users_;
};
}

int run_gui_app(int, char*[])
{
    GuiApp app;
    return app.run();
}
