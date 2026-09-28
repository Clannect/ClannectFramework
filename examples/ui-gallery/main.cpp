// The cfw-ui controls in a native window: the widgets that will replace the
// editor's Qt ones.
//
//     cfw-ui-gallery                      interactive
//     cfw-ui-gallery --screenshot out.png paints one frame, saves it, exits

#include <chrono>
#include <cstdio>

#include "cfw/app/UiWindow.h"
#include "cfw/image/Png.h"
#include "cfw/io/Arguments.h"
#include "cfw/io/FileSystem.h"
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

void build(Surface &surface, SceneModel &model) {
    auto &split = static_cast<Splitter &>(surface.root().add(std::make_unique<Splitter>(Stack::Direction::Row, 0.34f)));
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

    if (screenshot.empty()) {
        runUntilClosed(*ui);
        return 0;
    }
    // Let the window map, paint a frame, and read it back from the window system.
    for (int i = 0; i < 30; ++i) {
        processEvents(std::chrono::milliseconds(10));
        ui->frame();
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
