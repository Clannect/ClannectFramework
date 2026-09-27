#pragma once

#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"

namespace cfw {

// Command-line options, as QCommandLineParser reads them: "--name value",
// "--name=value" or "-name value" for options with a value, "--name" for
// flags, "--" ends the options, and everything else is positional. -h,
// --help and -? ask for help; -v and --version for the version.
//
//     CommandLine cl("Runs one Clannect Game Instance.", "1.0");
//     cl.addOption("port", "Port (default 7777).", "port", "7777");
//     cl.addOption("mint-dev-ticket", "Print a development ticket and exit.");
//     if (auto parsed = cl.parse(cfw::processArguments(argc, argv)); !parsed) { ... }
//     if (cl.helpRequested()) { std::fputs(cl.helpText().c_str(), stdout); return 0; }
//     const int port = std::stoi(cl.value("port"));
//
// Threads: a value type. Allocates: the options and parsed values.
class CommandLine {
public:
    explicit CommandLine(String description = {}, String version = {});

    // A flag if `valueName` is empty; otherwise the option takes a value, and
    // value() returns `defaultValue` while it is not given.
    void addOption(String name, String help, String valueName = {}, String defaultValue = {});

    // `args` includes the program name first (as argv does). Fails with
    // InvalidArgument on an unknown option, or on an option missing its value.
    Result<void> parse(Span<const String> args);

    [[nodiscard]] bool isSet(StringView name) const noexcept;
    // The last value given, or the default.
    [[nodiscard]] String value(StringView name) const;
    // Every value given, in order.
    [[nodiscard]] std::vector<String> values(StringView name) const;
    [[nodiscard]] const std::vector<String> &positional() const noexcept { return m_positional; }
    [[nodiscard]] const String &programName() const noexcept { return m_program; }

    [[nodiscard]] bool helpRequested() const noexcept { return m_help; }
    [[nodiscard]] bool versionRequested() const noexcept { return m_version; }
    // "Usage: program [options]", the description, and the options with their help.
    [[nodiscard]] String helpText() const;
    // "program version".
    [[nodiscard]] String versionText() const;

private:
    struct Option {
        String name;
        String help;
        String valueName;
        String defaultValue;
        std::vector<String> given;
        bool set = false;
    };
    const Option *find(StringView name) const noexcept;

    String m_description;
    String m_versionString;
    String m_program;
    std::vector<Option> m_options;
    std::vector<String> m_positional;
    bool m_help = false;
    bool m_version = false;
};

} // namespace cfw
