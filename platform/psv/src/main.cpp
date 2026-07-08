#include <borealis.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" void flockfile(FILE*) {}
extern "C" void funlockfile(FILE*) {}

namespace {

constexpr int kPort = 53317;
constexpr const char* kProtocol = "LocalSend protocol 2.1";
constexpr const char* kInboxPath = "ux0:data/localsend/inbox/";
constexpr const char* kConfigPath = "ux0:data/localsend/config.json";
constexpr const char* kLogPath = "ux0:data/localsend-borealis.log";

brls::Label* makeLabel(const std::string& text,
                       float size,
                       brls::HorizontalAlign align = brls::HorizontalAlign::LEFT,
                       float width = 780.0f)
{
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(size);
    label->setHorizontalAlign(align);
    label->setWidth(width);
    return label;
}

brls::Box* makeRow(const std::string& name, const std::string& value)
{
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setWidth(780);
    row->setMargins(0, 0, 10, 0);
    row->setAlignItems(brls::AlignItems::CENTER);

    auto* key = makeLabel(name, 20, brls::HorizontalAlign::LEFT, 230);
    auto* text = makeLabel(value, 20, brls::HorizontalAlign::LEFT, 520);
    text->setGrow(1.0f);

    row->addView(key);
    row->addView(text);
    return row;
}

brls::Label* makeSection(const std::string& text)
{
    auto* label = makeLabel(text, 24, brls::HorizontalAlign::LEFT, 780);
    label->setMargins(26, 0, 14, 0);
    return label;
}

brls::Box* makePanel()
{
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setGrow(1.0f);
    root->setPadding(34, 70, 34, 70);
    root->setAlignItems(brls::AlignItems::CENTER);

    auto* title = makeLabel("LocalSend Handheld", 38, brls::HorizontalAlign::CENTER, 780);
    title->setMargins(0, 0, 8, 0);
    root->addView(title);

    auto* subtitle = makeLabel("PS Vita UI base - borealis/GXM", 22, brls::HorizontalAlign::CENTER, 780);
    subtitle->setMargins(0, 0, 18, 0);
    root->addView(subtitle);

    root->addView(makeSection("Runtime"));
    root->addView(makeRow("Renderer", "borealis GXM"));
    root->addView(makeRow("Local port", std::to_string(kPort)));
    root->addView(makeRow("Protocol", kProtocol));
    root->addView(makeRow("Transport", "HTTP first; HTTPS integration pending on Vita"));

    root->addView(makeSection("Paths"));
    root->addView(makeRow("Inbox", kInboxPath));
    root->addView(makeRow("Config", kConfigPath));
    root->addView(makeRow("Log", kLogPath));

    root->addView(makeSection("Feature wiring"));
    root->addView(makeRow("Receive server", "Not started on Vita UI yet"));
    root->addView(makeRow("Discovery", "Not started on Vita UI yet"));
    root->addView(makeRow("File browser", "Core exists; controller UI pending"));

    auto* hint = makeLabel("Press START to exit", 20, brls::HorizontalAlign::CENTER, 780);
    hint->setMargins(24, 0, 0, 0);
    root->addView(hint);

    return root;
}

} // namespace

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    brls::Logger::setLogOutput(std::fopen(kLogPath, "w+"));
    brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_AUTO;

    if (!brls::Application::init())
        return EXIT_FAILURE;

    brls::Application::createWindow("LocalSend Handheld");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(true);

    auto* frame = new brls::AppletFrame(makePanel());
    frame->setTitle("LocalSend Handheld");
    brls::Application::pushActivity(new brls::Activity(frame));

    while (brls::Application::mainLoop())
        ;

    return EXIT_SUCCESS;
}
