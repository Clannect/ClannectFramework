# The screen reader's side of AtspiTest, through pyatspi (the library Orca
# uses): finds the application, walks its window, acts, and waits for a
# focus event. Prints one line per finding for the test to check.
import sys
import time

import pyatspi
from gi.repository import GLib

title = sys.argv[1]
desktop = pyatspi.Registry.getDesktop(0)
app = None
for _ in range(100):
    for candidate in desktop:
        if candidate is not None and candidate.name == title:
            app = candidate
    if app is not None:
        break
    time.sleep(0.1)
print("APP", repr(app.name if app is not None else None), app.childCount if app else 0, flush=True)
if app is None:
    sys.exit(1)

STATES = ["focusable", "focused", "checked", "editable", "expandable", "expanded", "selected", "enabled", "showing"]


def walk(node, depth):
    state = node.getState()
    names = [s for s in STATES if state.contains(getattr(pyatspi, "STATE_" + s.upper()))]
    value = ""
    try:
        value = node.queryText().getText(0, -1)
    except NotImplementedError:
        pass
    print("NODE", depth, node.getRoleName(), repr(node.name), ",".join(names), repr(value), flush=True)
    for child in node:
        walk(child, depth + 1)


def find(node, name, role):
    if node.name == name and node.getRoleName() == role:
        return node
    for child in node:
        found = find(child, name, role)
        if found is not None:
            return found
    return None


frame = app[0]
print("FRAME", frame.getRoleName(), repr(frame.name), frame.getIndexInParent(), repr(frame.parent.name), flush=True)
walk(frame, 0)

button = find(frame, "Save", "push button")
action = button.queryAction()
print("ACTION", action.nActions, action.getName(0), action.doAction(0), flush=True)
extents = button.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
print("EXTENTS", extents.x, extents.y, extents.width, extents.height, flush=True)
field = find(frame, "Name", "text")
print("SETTEXT", field.queryEditableText().setTextContents("Wall"), flush=True)
print("TEXTNOW", repr(field.queryText().getText(0, -1)), flush=True)
spin = find(frame, "Size", "spin button")
value = spin.queryValue()
print("VALUE", value.currentValue, value.minimumValue, value.maximumValue, flush=True)
value.currentValue = 7.0
workspace = find(frame, "Workspace", "tree item")
tree_action = workspace.queryAction()
print("EXPAND", tree_action.getName(tree_action.nActions - 1), tree_action.doAction(tree_action.nActions - 1), flush=True)
print("CHILD", repr(find(frame, "Baseplate", "tree item").name if find(frame, "Baseplate", "tree item") else None), flush=True)

focused = []


def on_focus(event):
    if event.detail1 == 1:
        focused.append(event.source.name)


pyatspi.Registry.registerEventListener(on_focus, "object:state-changed:focused")
print("LISTENING", flush=True)
loop = GLib.MainLoop()


def check():
    if focused:
        loop.quit()
        return False
    return True


GLib.timeout_add(20, check)
GLib.timeout_add(8000, loop.quit)
loop.run()
print("FOCUS", repr(focused[0] if focused else None), flush=True)
