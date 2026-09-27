#include "cfw/core/CommandLine.h"

#include <algorithm>
#include <optional>

namespace cfw {

CommandLine::CommandLine(String description, String version)
    : m_description(std::move(description)), m_versionString(std::move(version)) {}

void CommandLine::addOption(String name, String help, String valueName, String defaultValue) {
    m_options.push_back({std::move(name), std::move(help), std::move(valueName), std::move(defaultValue), {}, false});
}

const CommandLine::Option *CommandLine::find(StringView name) const noexcept {
    for (const Option &o : m_options) {
        if (o.name == name) {
            return &o;
        }
    }
    return nullptr;
}

Result<void> CommandLine::parse(Span<const String> args) {
    for (Option &o : m_options) {
        o.given.clear();
        o.set = false;
    }
    m_positional.clear();
    m_help = false;
    m_version = false;
    m_program = args.empty() ? String() : args[0];
    bool optionsEnded = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const String &arg = args[i];
        if (optionsEnded || arg.size() < 2 || arg[0] != '-') {
            m_positional.push_back(arg);
            continue;
        }
        if (arg == "--") {
            optionsEnded = true;
            continue;
        }
        StringView body = StringView(arg).substr(arg[1] == '-' ? 2 : 1);
        std::optional<String> inlineValue;
        if (const std::size_t eq = body.find('='); eq != StringView::npos) {
            inlineValue = String(body.substr(eq + 1));
            body = body.substr(0, eq);
        }
        Option *option = const_cast<Option *>(find(body));
        if (!option) {
            if (body == "h" || body == "help" || body == "?") {
                m_help = true;
                continue;
            }
            if (body == "v" || body == "version") {
                m_version = true;
                continue;
            }
            return Error(ErrorCode::InvalidArgument, "Unknown option '" + String(body) + "'.").with("option", String(body));
        }
        option->set = true;
        if (option->valueName.empty()) {
            if (inlineValue) {
                return Error(ErrorCode::InvalidArgument, "Unexpected value after '" + option->name + "'.")
                    .with("option", option->name);
            }
            continue;
        }
        if (inlineValue) {
            option->given.push_back(std::move(*inlineValue));
        } else if (i + 1 < args.size()) {
            option->given.push_back(args[++i]);
        } else {
            return Error(ErrorCode::InvalidArgument, "Missing value after '" + String(arg) + "'.")
                .with("option", option->name);
        }
    }
    return {};
}

bool CommandLine::isSet(StringView name) const noexcept {
    const Option *o = find(name);
    return o && o->set;
}

String CommandLine::value(StringView name) const {
    const Option *o = find(name);
    if (!o) {
        return {};
    }
    return o->given.empty() ? o->defaultValue : o->given.back();
}

std::vector<String> CommandLine::values(StringView name) const {
    const Option *o = find(name);
    return o ? o->given : std::vector<String>{};
}

String CommandLine::helpText() const {
    String out = "Usage: " + (m_program.empty() ? String("program") : m_program) + " [options]\n";
    if (!m_description.empty()) {
        out += m_description + "\n";
    }
    std::vector<std::pair<String, String>> rows;
    rows.emplace_back("-h, --help", "Displays help on commandline options.");
    if (!m_versionString.empty()) {
        rows.emplace_back("-v, --version", "Displays version information.");
    }
    for (const Option &o : m_options) {
        String left = "--" + o.name;
        if (!o.valueName.empty()) {
            left += " <" + o.valueName + ">";
        }
        rows.emplace_back(std::move(left), o.help);
    }
    std::size_t width = 0;
    for (const auto &row : rows) {
        width = std::max(width, row.first.size());
    }
    out += "\nOptions:\n";
    for (const auto &[left, help] : rows) {
        out += "  " + left + String(width - left.size() + 2, ' ') + help + "\n";
    }
    return out;
}

String CommandLine::versionText() const { return m_program + " " + m_versionString + "\n"; }

} // namespace cfw
