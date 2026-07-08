#include <borealis.hpp>

#include <cstdlib>
#include <cstdio>

namespace {

brls::Label* makeLabel(const std::string& text, float size, brls::HorizontalAlign align)
{
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(size);
    label->setHorizontalAlign(align);
    label->setWidth(780);
    return label;
}

brls::View* makeSmokeView()
{
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setGrow(1.0f);
    root->setPadding(54);
    root->setJustifyContent(brls::JustifyContent::CENTER);
    root->setAlignItems(brls::AlignItems::CENTER);

    auto* title = makeLabel("LocalSend Handheld", 44, brls::HorizontalAlign::CENTER);
    auto* status = makeLabel("PS Vita borealis/GXM smoke screen", 25, brls::HorizontalAlign::CENTER);
    auto* hint = makeLabel("START exits. Protocol core is not wired on PSV yet.", 22, brls::HorizontalAlign::CENTER);

    root->addView(title);
    root->addView(status);
    root->addView(hint);
    return root;
}

} // namespace

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    brls::Logger::setLogOutput(std::fopen("ux0:data/localsend-borealis.log", "w+"));
    brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_AUTO;

    if (!brls::Application::init())
        return EXIT_FAILURE;

    brls::Application::createWindow("LocalSend Handheld");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(true);

    auto* frame = new brls::AppletFrame(makeSmokeView());
    frame->setTitle("LocalSend Handheld");
    brls::Application::pushActivity(new brls::Activity(frame));

    while (brls::Application::mainLoop())
        ;

    return EXIT_SUCCESS;
}
