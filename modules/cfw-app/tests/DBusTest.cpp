// The D-Bus client: values of every type encode and decode to themselves
// (with the specification's alignment), bad input is refused, and against a
// real dbus-daemon two connections authenticate, get names, and call a
// method one of them serves, including an error reply.
// The daemon part skips without dbus-daemon.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/linux/DBus.h"
#include "cfw/test/Check.h"

using namespace cfw;
using namespace cfw::dbus;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

void roundTrip() {
    Message m = Message::methodCall("org.example", "/org/example/obj", "org.example.Iface", "Do");
    m.serial = 7;
    m.body = {
        Value::byte(200),
        Value::boolean(true),
        Value::int16(-3),
        Value::uint32(0xDEADBEEF),
        Value::int64(-1234567890123),
        Value::dbl(2.5),
        Value::string("héllo"),
        Value::objectPath("/a/b"),
        Value::array("(so)", {Value::structure({Value::string(":1.5"), Value::objectPath("/x")}),
                              Value::structure({Value::string(":1.6"), Value::objectPath("/y")})}),
        Value::array("{sv}", {Value::dictEntry(Value::string("level"), Value::variant(Value::int32(2)))}),
        Value::array("u", {Value::uint32(1), Value::uint32(2)}),
        Value::array("s"),
        Value::variant(Value::structure({Value::int32(1), Value::byte(2)})),
    };
    const std::vector<std::uint8_t> bytes = encode(m);
    auto decoded = decode(bytes.data(), bytes.size());
    check(decoded.ok() && decoded.value().has_value(), "decodes");
    if (!decoded.ok() || !decoded.value()) {
        return;
    }
    const Message &back = decoded.value()->first;
    checkEqual(decoded.value()->second, bytes.size(), "the whole message");
    checkEqual(back.path, String("/org/example/obj"), "the path");
    checkEqual(back.member, String("Do"), "the member");
    checkEqual(back.serial, std::uint32_t(7), "the serial");
    checkEqual(back.bodySignature(), m.bodySignature(), "the body's signature");
    checkEqual(back.bodySignature(), String("ybnuxdsoa(so)a{sv}auasv"), "which is right");
    check(back.body.size() == m.body.size(), "every value");
    if (back.body.size() == m.body.size()) {
        checkEqual(back.body[0].integer, std::int64_t(200), "a byte");
        checkEqual(back.body[3].unsignedInteger, std::uint64_t(0xDEADBEEF), "a uint32");
        checkEqual(back.body[4].integer, std::int64_t(-1234567890123), "an int64");
        checkEqual(back.body[5].real, 2.5, "a double");
        checkEqual(back.body[6].text, String("héllo"), "a UTF-8 string");
        checkEqual(back.body[8].items.size(), std::size_t(2), "an array of structs");
        checkEqual(back.body[8].items[1].items[1].text, String("/y"), "their fields");
        checkEqual(back.body[9].items[0].items[1].asInt(), std::int64_t(2), "a dictionary's variant");
        checkEqual(back.body[11].items.size(), std::size_t(0), "an empty array");
        checkEqual(back.body[12].items[0].items[1].integer, std::int64_t(2), "a variant holding a struct");
    }
    // Incomplete data waits; garbage fails.
    auto partial = decode(bytes.data(), bytes.size() - 1);
    check(partial.ok() && !partial.value().has_value(), "a partial message waits for more");
    std::vector<std::uint8_t> bad = bytes;
    bad[0] = 'X';
    check(!decode(bad.data(), bad.size()).ok(), "a bad endianness mark fails");
    std::vector<std::uint8_t> huge = bytes;
    huge[4] = 0xFF;
    huge[5] = 0xFF;
    huge[6] = 0xFF;
    huge[7] = 0x7F;
    check(!decode(huge.data(), huge.size()).ok(), "an absurd length fails");
}

bool haveDaemon() { return std::system("command -v dbus-daemon >/dev/null 2>&1") == 0; }

void againstDaemon() {
    if (!haveDaemon()) {
        std::printf("DBusTest: the daemon part skipped (no dbus-daemon)\n");
        return;
    }
    int out[2];
    if (::pipe(out) != 0) {
        return;
    }
    const pid_t daemon = fork();
    if (daemon == 0) {
        dup2(out[1], 1);
        ::close(out[0]);
        execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork", "--print-address", static_cast<char *>(nullptr));
        _exit(127);
    }
    ::close(out[1]);
    std::string address;
    char c = 0;
    while (::read(out[0], &c, 1) == 1 && c != '\n') {
        address += c;
    }
    check(!address.empty(), "the daemon prints its address");

    auto server = Connection::open(address, std::chrono::seconds(3));
    auto client = Connection::open(address, std::chrono::seconds(3));
    check(server.ok() && client.ok(), "two connections authenticate");
    if (server.ok() && client.ok()) {
        check(server.value()->uniqueName().rfind(":1.", 0) == 0, "and get unique names");
        Connection &s = *server.value();
        Connection &cl = *client.value();
        s.onMethodCall = [&s](const Message &call) {
            if (call.member == "Echo") {
                s.send(Message::methodReturn(call, call.body));
            } else {
                s.send(Message::error(call, "org.example.Error.Nope", "no such thing"));
            }
        };
        Result<Message> names = cl.call(
            Message::methodCall("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames"),
            std::chrono::seconds(2));
        check(names.ok() && !names.value().body.empty() && names.value().body[0].items.size() >= 3,
              "the bus lists its names");
        // The server answers from another process (the socket is shared by
        // fork), so the client's blocking calls have someone to talk to.
        const pid_t serving = fork();
        if (serving == 0) {
            for (int i = 0; i < 500; ++i) {
                s.dispatch();
                usleep(2000);
            }
            _exit(0);
        }
        Result<Message> echo = cl.call(Message::methodCall(s.uniqueName(), "/test", "org.example.Test", "Echo",
                                                           {Value::string("pong"), Value::array("i", {Value::int32(4)})}),
                                       std::chrono::seconds(3));
        check(echo.ok() && echo.value().body.size() == 2 && echo.value().body[0].text == "pong" &&
                  echo.value().body[1].items.size() == 1,
              "a served method answers with its values");
        Result<Message> failing =
            cl.call(Message::methodCall(s.uniqueName(), "/test", "org.example.Test", "Nope"), std::chrono::seconds(3));
        check(!failing.ok() && failing.error().message().find("org.example.Error.Nope") != std::string::npos,
              "an error reply is an error, with its name");
        kill(serving, SIGTERM);
        waitpid(serving, nullptr, 0);
    }
    kill(daemon, SIGTERM);
    waitpid(daemon, nullptr, 0);
    ::close(out[0]);
}

} // namespace

int main() {
    roundTrip();
    againstDaemon();
    return cfw::test::finish("DBusTest");
}
