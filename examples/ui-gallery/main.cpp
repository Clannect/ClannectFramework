// The cfw-ui controls in a native window: the widgets that will replace the
// editor's Qt ones.
//
//     cfw-ui-gallery                      interactive
//     cfw-ui-gallery --screenshot out.png paints one frame, saves it, exits
//     (add --menu to show the File menu with its submenu open)

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "cfw/app/UiWindow.h"
#include "cfw/image/Png.h"
#include "cfw/io/Arguments.h"
#include "cfw/io/FileSystem.h"
#include "cfw/ui/Chrome.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Views.h"

using namespace cfw;

namespace {

// A scene-like tree: Workspace, Lighting... with parts under them.
class SceneModel : public TreeModel {
public:
    std::size_t childCount(Id parent) const override { return parent == kRoot ? 6 : parent <= 6 ? 250 : 0; }
    Id child(Id parent, std::size_t i) const override { return parent == kRoot ? Id(i + 1) : parent * 1000 + i + 1; }
    String text(Id node) const override {
        static const char *services[] = {"Workspace", "Lighting", "ReplicatedStorage", "StarterGui", "StarterPlayer", "SoundService"};
        return node <= 6 ? String(services[node - 1]) : "Part" + std::to_string(node % 1000);
    }
};

// A Lucide outline icon (https://lucide.dev, ISC), drawn in the text colour.
Icon lucide(const char *body) {
    return Icon::fromSvg(String(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" )"
                                R"(stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">)") +
                         body + "</svg>");
}

void build(Surface &surface, SceneModel &model) {
    auto &window = static_cast<Stack &>(surface.root().add(std::make_unique<Stack>(Stack::Direction::Column, 0.0f)));

    const Icon save = lucide(R"(<path d="M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/><path d="M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7"/><path d="M7 3v4a1 1 0 0 0 1 1h7"/>)");
    const Icon undo = lucide(R"(<path d="M9 14 4 9l5-5"/><path d="M4 9h10.5a5.5 5.5 0 0 1 5.5 5.5a5.5 5.5 0 0 1-5.5 5.5H11"/>)");
    const Icon redo = lucide(R"(<path d="m15 14 5-5-5-5"/><path d="M20 9H9.5A5.5 5.5 0 0 0 4 14.5A5.5 5.5 0 0 0 9.5 20H13"/>)");
    const Icon select = lucide(R"(<path d="M4.037 4.688a.495.495 0 0 1 .651-.651l16 6.5a.5.5 0 0 1-.063.947l-6.124 1.58a2 2 0 0 0-1.438 1.435l-1.579 6.126a.5.5 0 0 1-.947.063z"/>)");
    const Icon move = lucide(R"(<path d="M12 2v20"/><path d="m15 19-3 3-3-3"/><path d="m19 9 3 3-3 3"/><path d="M2 12h20"/><path d="m5 9-3 3 3 3"/><path d="m9 5 3-3 3 3"/>)");
    const Icon play = lucide(R"(<polygon points="6 3 20 12 6 21 6 3"/>)");

    auto &menus = window.add<MenuBar>();
    menus.addMenu("&File", [save](Menu &m) {
        m.addItem(save, "Save", [] {}, "Ctrl+S");
        m.addSubmenu("Recent Places", [](Menu &recent) {
            recent.addItem("Obby", [] {});
            recent.addItem("Baseplate", [] {});
        });
        m.addSeparator();
        m.addItem("Exit", [] {});
    });
    menus.addMenu("&Edit", [undo, redo](Menu &m) {
        m.addItem(undo, "Undo", [] {}, "Ctrl+Z");
        m.addItem(redo, "Redo", [] {}, "Ctrl+Y");
    });
    menus.addMenu("&View", [](Menu &m) { m.addItem("Output", [] {}).setChecked(true); });
    menus.addMenu("&Help", [](Menu &m) { m.addItem("About", [] {}); });

    auto &tools = window.add<ToolBar>();
    tools.addButton(save, "Save (Ctrl+S)", [] {});
    tools.addButton(undo, "Undo (Ctrl+Z)", [] {});
    tools.addButton(redo, "Redo (Ctrl+Y)", [] {});
    tools.addSeparator();
    tools.addToggle(select, "Select", [](bool) {}, true);
    tools.addToggle(move, "Move", [](bool) {});
    tools.addSpacer();
    tools.addButton(play, "Play (F5)", [] {});

    auto &split = static_cast<Splitter &>(window.add(std::make_unique<Splitter>(Stack::Direction::Row, 0.34f)));
    split.setStretch(1);
    auto &tree = static_cast<TreeView &>(split.setFirst(std::make_unique<TreeView>(model)));
    tree.setExpanded(1, true);
    tree.setSelection({1003});

    auto &right = static_cast<Stack &>(split.setSecond(std::make_unique<Stack>(Stack::Direction::Column, 0.0f)));
    right.add<TabBar>(std::vector<String>{"Properties", "Attributes", "Script"});
    auto &scroll = right.add<ScrollArea>();
    scroll.setStretch(1);
    auto &form = scroll.setContent<Stack>(Stack::Direction::Column, 10.0f, 16.0f);

    Label &title = form.add<Label>("Clannect Framework controls");
    title.setAccessibleName("title");
    form.add<Label>("Every control below is drawn by cfw::Painter in a native window, no Qt.").setMuted(true);

    auto &buttons = form.add<Stack>(Stack::Direction::Row, 8.0f);
    buttons.add<Button>("Play").setPrimary(true);
    buttons.add<Button>("Publish");
    buttons.add<Button>("Disabled").setEnabled(false);

    auto &name = form.add<TextField>("Baseplate");
    name.setPlaceholder("Name");
    auto &empty = form.add<TextField>();
    empty.setPlaceholder("Search the Explorer…");

    auto &row = form.add<Stack>(Stack::Direction::Row, 8.0f);
    row.add<Label>("Transparency");
    auto &number = row.add<NumberField>(0.25, 2);
    number.setRange(0.0, 1.0);
    number.setStep(0.05);
    row.add<Dropdown>(std::vector<String>{"Plastic", "Wood", "Metal", "Neon"}, 0);

    form.add<CheckBox>("Anchored", true);
    form.add<CheckBox>("CanCollide", false);
    auto &sliderRow = form.add<Stack>(Stack::Direction::Row, 8.0f);
    sliderRow.add<Label>("Brightness");
    sliderRow.add<Slider>(0.0, 10.0, 3.0).setStretch(1);
    auto &progress = form.add<ProgressBar>();
    progress.setValue(0.62f);
    auto &wrapped = form.add<Label>("Text wraps to the width of its column, and the scroll area on the right "
                                    "appears when the content is taller than the window.");
    wrapped.setWrap(true);
    for (int i = 1; i <= 12; ++i) {
        form.add<Label>("Row " + std::to_string(i));
    }
}

} // namespace

int main(int argc, char **argv) {
    const std::vector<String> args = processArguments(argc, argv);
    String screenshot;
    for (std::size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--screenshot") {
            screenshot = args[i + 1];
        }
    }

    auto created = UiWindow::create({"Clannect Framework: controls", {960, 600}}, Theme::dark().withSystemFonts());
    if (!created) {
        std::fprintf(stderr, "cfw-ui-gallery: %s\n", created.error().message().c_str());
        return 1;
    }
    std::unique_ptr<UiWindow> ui = std::move(created).value();
    SceneModel model;
    build(ui->surface(), model);
    const bool showMenu = std::find(args.begin(), args.end(), "--menu") != args.end();

    if (screenshot.empty()) {
        runUntilClosed(*ui);
        return 0;
    }
    // Let the window map, paint a frame, and read it back from the window system.
    for (int i = 0; i < 30; ++i) {
        processEvents(std::chrono::milliseconds(10));
        ui->frame();
        if (showMenu && i == 5) {
            // Open File and hover its submenu, as a person would.
            Surface &surface = ui->surface();
            auto *bar = static_cast<MenuBar *>(static_cast<Stack &>(*surface.root().children()[0]).children()[0].get());
            bar->openMenu(0);
            surface.layout();
            Element *recent = surface.hitTest({bar->rect().x + 40, bar->rect().bottom() + 50});
            if (recent) {
                PointerEvent hover;
                hover.position = recent->rect().center();
                surface.dispatch(hover);
            }
        }
    }
    auto shot = ui->window().capture();
    if (!shot) {
        std::fprintf(stderr, "cfw-ui-gallery: %s\n", shot.error().message().c_str());
        return 1;
    }
    auto png = encodePng(shot.value());
    if (!png || !writeFileAtomic(Path(screenshot), png.value())) {
        std::fprintf(stderr, "cfw-ui-gallery: could not write %s\n", screenshot.c_str());
        return 1;
    }
    std::printf("wrote %s\n", screenshot.c_str());
    return 0;
}
