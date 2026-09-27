#include "cfw/gfx/SvgImage.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <numbers>

#include "cfw/gfx/Painter.h"

namespace cfw {

namespace {

// ---- Numbers and path data ----

struct Scanner {
    StringView s;
    std::size_t i = 0;

    void skipSpaceAndComma() {
        while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) {
            ++i;
        }
    }
    void skipSpace() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
    }
    bool done() {
        skipSpaceAndComma();
        return i >= s.size();
    }
    // An SVG number: sign, digits, fraction, exponent ("-.5e-3", "1.5.5" is two numbers).
    std::optional<float> number() {
        skipSpaceAndComma();
        const std::size_t start = i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            ++i;
        }
        bool digits = false;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            ++i;
            digits = true;
        }
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                ++i;
                digits = true;
            }
        }
        if (!digits) {
            i = start;
            return std::nullopt;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            std::size_t e = i + 1;
            if (e < s.size() && (s[e] == '+' || s[e] == '-')) {
                ++e;
            }
            if (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e]))) {
                i = e;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                    ++i;
                }
            }
        }
        const std::string text(s.substr(start, i - start));
        char *end = nullptr;
        const float v = std::strtof(text.c_str(), &end);
        return std::isfinite(v) ? std::optional<float>(v) : std::nullopt;
    }
    // An arc flag: a single 0 or 1, which need no separator ("a1 1 0 01 5 5").
    std::optional<bool> flag() {
        skipSpaceAndComma();
        if (i < s.size() && (s[i] == '0' || s[i] == '1')) {
            return s[i++] == '1';
        }
        return std::nullopt;
    }
};

// An elliptical arc from `from` to `to` as cubic Béziers (SVG 1.1 F.6).
void arcTo(PainterPath &path, Vec2 from, float rxIn, float ryIn, float rotation, bool large, bool sweep, Vec2 to) {
    double rx = std::abs(rxIn);
    double ry = std::abs(ryIn);
    if (from == to) {
        return;
    }
    if (rx == 0 || ry == 0) {
        path.lineTo(to);
        return;
    }
    const double phi = rotation * std::numbers::pi / 180.0;
    const double cp = std::cos(phi);
    const double sp = std::sin(phi);
    const double dx = (static_cast<double>(from.x) - to.x) / 2;
    const double dy = (static_cast<double>(from.y) - to.y) / 2;
    const double x1 = cp * dx + sp * dy;
    const double y1 = -sp * dx + cp * dy;
    const double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1) {
        rx *= std::sqrt(lambda);
        ry *= std::sqrt(lambda);
    }
    const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0.0;
    if (large == sweep) {
        coef = -coef;
    }
    const double cx1 = coef * rx * y1 / ry;
    const double cy1 = -coef * ry * x1 / rx;
    const double cx = cp * cx1 - sp * cy1 + (static_cast<double>(from.x) + to.x) / 2;
    const double cy = sp * cx1 + cp * cy1 + (static_cast<double>(from.y) + to.y) / 2;
    const auto angle = [](double ux, double uy, double vx, double vy) {
        return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    };
    const double theta1 = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
    double delta = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    if (!sweep && delta > 0) {
        delta -= 2 * std::numbers::pi;
    } else if (sweep && delta < 0) {
        delta += 2 * std::numbers::pi;
    }
    const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(delta) / (std::numbers::pi / 2) - 1e-9)));
    const double step = delta / segments;
    const double k = 4.0 / 3.0 * std::tan(step / 4);
    const auto point = [&](double t, double ox, double oy) {
        const double ex = rx * ox;
        const double ey = ry * oy;
        (void)t;
        return Vec2{static_cast<float>(cp * ex - sp * ey + cx), static_cast<float>(sp * ex + cp * ey + cy)};
    };
    double t = theta1;
    for (int n = 0; n < segments; ++n) {
        const double c0 = std::cos(t);
        const double s0 = std::sin(t);
        const double t2 = t + step;
        const double c1 = std::cos(t2);
        const double s1 = std::sin(t2);
        const Vec2 p1 = point(t, c0 - k * s0, s0 + k * c0);
        const Vec2 p2 = point(t2, c1 + k * s1, s1 - k * c1);
        const Vec2 p3 = n + 1 == segments ? to : point(t2, c1, s1);
        path.cubicTo(p1, p2, p3);
        t = t2;
    }
}

} // namespace

bool parseSvgPathData(StringView data, PainterPath &out) {
    Scanner sc{data};
    char command = 0;
    Vec2 current{};
    Vec2 start{};
    Vec2 lastControl{};
    char lastCommand = 0;
    while (!sc.done()) {
        const char c = sc.s[sc.i];
        if (std::isalpha(static_cast<unsigned char>(c))) {
            command = c;
            ++sc.i;
        } else if (command == 0) {
            return false;
        }
        const bool rel = std::islower(static_cast<unsigned char>(command));
        const Vec2 base = rel ? current : Vec2{};
        const auto pt = [&]() -> std::optional<Vec2> {
            const std::optional<float> x = sc.number();
            const std::optional<float> y = x ? sc.number() : std::nullopt;
            if (!y) {
                return std::nullopt;
            }
            return Vec2{base.x + *x, base.y + *y};
        };
        switch (std::toupper(static_cast<unsigned char>(command))) {
        case 'M': {
            const std::optional<Vec2> p = pt();
            if (!p) {
                return false;
            }
            out.moveTo(*p);
            current = start = *p;
            command = rel ? 'l' : 'L'; // further pairs are lines
            lastCommand = 'M';
            continue;
        }
        case 'Z':
            out.close();
            current = start;
            lastCommand = 'Z';
            command = 0; // a number may not follow
            if (!sc.done() && !std::isalpha(static_cast<unsigned char>(sc.s[sc.i]))) {
                return false;
            }
            continue;
        case 'L': {
            const std::optional<Vec2> p = pt();
            if (!p) {
                return false;
            }
            out.lineTo(*p);
            current = *p;
            break;
        }
        case 'H': {
            const std::optional<float> x = sc.number();
            if (!x) {
                return false;
            }
            current = {base.x + *x, current.y};
            out.lineTo(current);
            break;
        }
        case 'V': {
            const std::optional<float> y = sc.number();
            if (!y) {
                return false;
            }
            current = {current.x, (rel ? current.y : 0.0f) + *y};
            out.lineTo(current);
            break;
        }
        case 'C':
        case 'S': {
            Vec2 c1;
            if (std::toupper(static_cast<unsigned char>(command)) == 'C') {
                const std::optional<Vec2> p = pt();
                if (!p) {
                    return false;
                }
                c1 = *p;
            } else {
                const char lc = static_cast<char>(std::toupper(static_cast<unsigned char>(lastCommand)));
                c1 = (lc == 'C' || lc == 'S') ? Vec2{2 * current.x - lastControl.x, 2 * current.y - lastControl.y} : current;
            }
            const std::optional<Vec2> c2 = pt();
            const std::optional<Vec2> p = c2 ? pt() : std::nullopt;
            if (!p) {
                return false;
            }
            out.cubicTo(c1, *c2, *p);
            lastControl = *c2;
            current = *p;
            break;
        }
        case 'Q':
        case 'T': {
            Vec2 c1;
            if (std::toupper(static_cast<unsigned char>(command)) == 'Q') {
                const std::optional<Vec2> p = pt();
                if (!p) {
                    return false;
                }
                c1 = *p;
            } else {
                const char lc = static_cast<char>(std::toupper(static_cast<unsigned char>(lastCommand)));
                c1 = (lc == 'Q' || lc == 'T') ? Vec2{2 * current.x - lastControl.x, 2 * current.y - lastControl.y} : current;
            }
            const std::optional<Vec2> p = pt();
            if (!p) {
                return false;
            }
            out.quadTo(c1, *p);
            lastControl = c1;
            current = *p;
            break;
        }
        case 'A': {
            const std::optional<float> rx = sc.number();
            const std::optional<float> ry = rx ? sc.number() : std::nullopt;
            const std::optional<float> rot = ry ? sc.number() : std::nullopt;
            const std::optional<bool> large = rot ? sc.flag() : std::nullopt;
            const std::optional<bool> sweep = large ? sc.flag() : std::nullopt;
            const std::optional<Vec2> p = sweep ? pt() : std::nullopt;
            if (!p) {
                return false;
            }
            arcTo(out, current, *rx, *ry, *rot, *large, *sweep, *p);
            current = *p;
            break;
        }
        default: return false;
        }
        lastCommand = command;
    }
    return true;
}

// ---- The document ----

namespace {

struct Attribute {
    StringView name;
    StringView value;
};

struct Style {
    SvgImage::Paint fill{SvgImage::Paint::Kind::Color, Color{0, 0, 0, 1}};
    SvgImage::Paint stroke{};
    float fillOpacity = 1;
    float strokeOpacity = 1;
    float opacity = 1;
    FillRule fillRule = FillRule::NonZero;
    float strokeWidth = 1;
    CapStyle cap = CapStyle::Flat;
    JoinStyle join = JoinStyle::SvgMiter;
    float miterLimit = 4;
    Transform2D transform;
};

StringView trim(StringView v) {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) {
        v.remove_prefix(1);
    }
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) {
        v.remove_suffix(1);
    }
    return v;
}

bool iequals(StringView a, StringView b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::optional<float> number(StringView v) {
    Scanner sc{trim(v)};
    const std::optional<float> n = sc.number();
    return n; // units such as "px" are ignored
}

std::optional<SvgImage::Paint> paint(StringView v) {
    v = trim(v);
    using Kind = SvgImage::Paint::Kind;
    if (v == "none" || v == "transparent") {
        return SvgImage::Paint{};
    }
    if (iequals(v, "currentColor")) {
        return SvgImage::Paint{Kind::CurrentColor, {}};
    }
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    if (!v.empty() && v[0] == '#') {
        std::uint8_t rgb[3] = {};
        if (v.size() == 4 || v.size() == 7) {
            const bool shortForm = v.size() == 4;
            for (int k = 0; k < 3; ++k) {
                const int a = hex(v[static_cast<std::size_t>(1 + (shortForm ? k : 2 * k))]);
                const int b = shortForm ? a : hex(v[static_cast<std::size_t>(2 + 2 * k)]);
                if (a < 0 || b < 0) {
                    return std::nullopt;
                }
                rgb[k] = static_cast<std::uint8_t>(a * 16 + b);
            }
            return SvgImage::Paint{Kind::Color, Color::fromRgba8(rgb[0], rgb[1], rgb[2])};
        }
        return std::nullopt;
    }
    if (v.size() > 5 && iequals(v.substr(0, 4), "rgb(") && v.back() == ')') {
        Scanner sc{v.substr(4, v.size() - 5)};
        float c[3] = {};
        for (float &x : c) {
            const std::optional<float> n = sc.number();
            if (!n) {
                return std::nullopt;
            }
            sc.skipSpace();
            const bool percent = sc.i < sc.s.size() && sc.s[sc.i] == '%';
            if (percent) {
                ++sc.i;
            }
            x = std::clamp(percent ? *n / 100.0f : *n / 255.0f, 0.0f, 1.0f);
        }
        return SvgImage::Paint{Kind::Color, Color{c[0], c[1], c[2], 1}};
    }
    static const struct {
        const char *name;
        std::uint8_t r, g, b;
    } kNamed[] = {{"black", 0, 0, 0},       {"white", 255, 255, 255}, {"red", 255, 0, 0},     {"green", 0, 128, 0},
                  {"blue", 0, 0, 255},      {"yellow", 255, 255, 0},  {"cyan", 0, 255, 255},  {"magenta", 255, 0, 255},
                  {"gray", 128, 128, 128},  {"grey", 128, 128, 128},  {"orange", 255, 165, 0}, {"purple", 128, 0, 128},
                  {"silver", 192, 192, 192}, {"lime", 0, 255, 0},     {"navy", 0, 0, 128},    {"maroon", 128, 0, 0}};
    for (const auto &n : kNamed) {
        if (iequals(v, n.name)) {
            return SvgImage::Paint{Kind::Color, Color::fromRgba8(n.r, n.g, n.b)};
        }
    }
    return std::nullopt;
}

std::optional<Transform2D> transformList(StringView v) {
    Transform2D t;
    Scanner sc{v};
    while (!sc.done()) {
        const std::size_t nameStart = sc.i;
        while (sc.i < sc.s.size() && std::isalpha(static_cast<unsigned char>(sc.s[sc.i]))) {
            ++sc.i;
        }
        const StringView name = sc.s.substr(nameStart, sc.i - nameStart);
        sc.skipSpace();
        if (sc.i >= sc.s.size() || sc.s[sc.i] != '(') {
            return std::nullopt;
        }
        ++sc.i;
        float a[6] = {};
        int n = 0;
        while (n < 6) {
            const std::optional<float> x = sc.number();
            if (!x) {
                break;
            }
            a[n++] = *x;
        }
        sc.skipSpaceAndComma();
        if (sc.i >= sc.s.size() || sc.s[sc.i] != ')') {
            return std::nullopt;
        }
        ++sc.i;
        Transform2D m;
        if (name == "matrix" && n == 6) {
            m = Transform2D::fromRows(a[0], a[2], a[4], a[1], a[3], a[5]);
        } else if (name == "translate" && (n == 1 || n == 2)) {
            m = Transform2D::translation(a[0], n == 2 ? a[1] : 0);
        } else if (name == "scale" && (n == 1 || n == 2)) {
            m = Transform2D::scaling(a[0], n == 2 ? a[1] : a[0]);
        } else if (name == "rotate" && (n == 1 || n == 3)) {
            m = Transform2D::rotation(a[0]);
            if (n == 3) {
                m = Transform2D::translation(-a[1], -a[2]).then(m).then(Transform2D::translation(a[1], a[2]));
            }
        } else if (name == "skewX" && n == 1) {
            m = Transform2D::fromRows(1, std::tan(a[0] * std::numbers::pi / 180), 0, 0, 1, 0);
        } else if (name == "skewY" && n == 1) {
            m = Transform2D::fromRows(1, 0, 0, std::tan(a[0] * std::numbers::pi / 180), 1, 0);
        } else {
            return std::nullopt;
        }
        t = t * m; // later transforms apply first
    }
    return t;
}

void applyAttribute(Style &st, StringView name, StringView value) {
    value = trim(value);
    if (name == "fill") {
        if (const std::optional<SvgImage::Paint> p = paint(value)) {
            st.fill = *p;
        }
    } else if (name == "stroke") {
        if (const std::optional<SvgImage::Paint> p = paint(value)) {
            st.stroke = *p;
        }
    } else if (name == "stroke-width") {
        if (const std::optional<float> w = number(value); w && *w >= 0) {
            st.strokeWidth = *w;
        }
    } else if (name == "stroke-linecap") {
        st.cap = value == "round" ? CapStyle::Round : value == "square" ? CapStyle::Square : CapStyle::Flat;
    } else if (name == "stroke-linejoin") {
        st.join = value == "round" ? JoinStyle::Round : value == "bevel" ? JoinStyle::Bevel : JoinStyle::SvgMiter;
    } else if (name == "stroke-miterlimit") {
        if (const std::optional<float> m = number(value); m && *m >= 1) {
            st.miterLimit = *m;
        }
    } else if (name == "fill-rule") {
        st.fillRule = value == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
    } else if (name == "opacity" || name == "fill-opacity" || name == "stroke-opacity") {
        if (const std::optional<float> o = number(value)) {
            const float v = std::clamp(*o, 0.0f, 1.0f);
            (name == "opacity" ? st.opacity : name == "fill-opacity" ? st.fillOpacity : st.strokeOpacity) =
                name == "opacity" ? st.opacity * v : v;
        }
    } else if (name == "style") {
        std::size_t start = 0;
        while (start < value.size()) {
            std::size_t end = value.find(';', start);
            if (end == StringView::npos) {
                end = value.size();
            }
            const StringView decl = value.substr(start, end - start);
            if (const std::size_t colon = decl.find(':'); colon != StringView::npos) {
                applyAttribute(st, trim(decl.substr(0, colon)), decl.substr(colon + 1));
            }
            start = end + 1;
        }
    }
}

// A minimal XML tag reader: yields start tags (with attributes), end tags,
// and skips text, comments, processing instructions and CDATA.
struct Tag {
    StringView name;
    std::vector<Attribute> attributes;
    bool end = false;         // </name>
    bool selfClosing = false; // <name/>
};

struct XmlReader {
    StringView s;
    std::size_t i = 0;
    bool ok = true;

    bool next(Tag &tag) {
        tag.attributes.clear();
        while (true) {
            const std::size_t lt = s.find('<', i);
            if (lt == StringView::npos) {
                return false;
            }
            i = lt + 1;
            if (s.substr(i, 3) == "!--") {
                const std::size_t e = s.find("-->", i + 3);
                if (e == StringView::npos) {
                    ok = false;
                    return false;
                }
                i = e + 3;
                continue;
            }
            if (s.substr(i, 8) == "![CDATA[") {
                const std::size_t e = s.find("]]>", i);
                i = e == StringView::npos ? s.size() : e + 3;
                continue;
            }
            if (i < s.size() && (s[i] == '?' || s[i] == '!')) {
                const std::size_t e = s.find('>', i);
                i = e == StringView::npos ? s.size() : e + 1;
                continue;
            }
            break;
        }
        tag.end = i < s.size() && s[i] == '/';
        if (tag.end) {
            ++i;
        }
        const std::size_t nameStart = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '>' && s[i] != '/') {
            ++i;
        }
        tag.name = s.substr(nameStart, i - nameStart);
        if (const std::size_t colon = tag.name.find(':'); colon != StringView::npos) {
            tag.name = tag.name.substr(colon + 1); // svg:path
        }
        tag.selfClosing = false;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
                ++i;
            }
            if (i < s.size() && s[i] == '>') {
                ++i;
                return true;
            }
            if (s.substr(i, 2) == "/>") {
                i += 2;
                tag.selfClosing = true;
                return true;
            }
            const std::size_t an = i;
            while (i < s.size() && s[i] != '=' && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '>' && s[i] != '/') {
                ++i;
            }
            const StringView name = s.substr(an, i - an);
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
                ++i;
            }
            if (i >= s.size() || s[i] != '=') {
                if (name.empty()) {
                    ok = false;
                    return false;
                }
                continue; // an attribute without a value
            }
            ++i;
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
                ++i;
            }
            if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) {
                ok = false;
                return false;
            }
            const char q = s[i++];
            const std::size_t e = s.find(q, i);
            if (e == StringView::npos) {
                ok = false;
                return false;
            }
            tag.attributes.push_back({name, s.substr(i, e - i)});
            i = e + 1;
        }
        ok = false;
        return false;
    }
};

StringView attribute(const Tag &t, StringView name) {
    for (const Attribute &a : t.attributes) {
        if (a.name == name) {
            return a.value;
        }
    }
    return {};
}

float attributeNumber(const Tag &t, StringView name, float fallback = 0) {
    const StringView v = attribute(t, name);
    return v.empty() ? fallback : number(v).value_or(fallback);
}

bool points(StringView v, PainterPath &path, bool close) {
    Scanner sc{v};
    std::vector<Vec2> pts;
    while (!sc.done()) {
        const std::optional<float> x = sc.number();
        const std::optional<float> y = x ? sc.number() : std::nullopt;
        if (!y) {
            break; // an odd number: the last is ignored
        }
        pts.push_back({*x, *y});
    }
    if (pts.size() < 2) {
        return false;
    }
    path.addPolygon(pts, close);
    return true;
}

} // namespace

Result<SvgImage> SvgImage::parse(StringView svg) {
    SvgImage image;
    XmlReader xml{svg};
    Tag tag;
    std::vector<Style> styles;
    int skipDepth = 0;     // inside an element whose content is ignored
    bool sawSvg = false;
    while (xml.next(tag)) {
        if (tag.end) {
            if (skipDepth > 0) {
                --skipDepth;
            } else if (!styles.empty()) {
                styles.pop_back();
            }
            continue;
        }
        if (skipDepth > 0) {
            skipDepth += tag.selfClosing ? 0 : 1;
            continue;
        }
        if (!sawSvg) {
            if (tag.name != "svg") {
                return Error(ErrorCode::ParseError, "not an SVG document");
            }
            sawSvg = true;
            const StringView vb = attribute(tag, "viewBox");
            Scanner sc{vb};
            const std::optional<float> x = sc.number();
            const std::optional<float> y = x ? sc.number() : std::nullopt;
            const std::optional<float> w = y ? sc.number() : std::nullopt;
            const std::optional<float> h = w ? sc.number() : std::nullopt;
            if (h && *w > 0 && *h > 0) {
                image.m_viewBox = {*x, *y, *w, *h};
            } else {
                image.m_viewBox = {0, 0, attributeNumber(tag, "width", 100), attributeNumber(tag, "height", 100)};
            }
        }
        const bool container = tag.name == "svg" || tag.name == "g";
        const bool shape = tag.name == "path" || tag.name == "rect" || tag.name == "circle" || tag.name == "ellipse" ||
                           tag.name == "line" || tag.name == "polyline" || tag.name == "polygon";
        if (!container && !shape) {
            skipDepth = tag.selfClosing ? 0 : 1; // defs, title, text, ...
            continue;
        }
        Style st = styles.empty() ? Style{} : styles.back();
        for (const Attribute &a : tag.attributes) {
            if (a.name == "transform") {
                if (const std::optional<Transform2D> t = transformList(a.value)) {
                    st.transform = st.transform * *t;
                }
            } else {
                applyAttribute(st, a.name, a.value);
            }
        }
        if (shape) {
            Shape s;
            bool ok = true;
            if (tag.name == "path") {
                (void)parseSvgPathData(attribute(tag, "d"), s.path);
            } else if (tag.name == "rect") {
                const float w = attributeNumber(tag, "width");
                const float h = attributeNumber(tag, "height");
                float rx = attributeNumber(tag, "rx", -1);
                float ry = attributeNumber(tag, "ry", -1);
                rx = rx < 0 ? (ry < 0 ? 0 : ry) : rx;
                ry = ry < 0 ? rx : ry;
                const RectF r{attributeNumber(tag, "x"), attributeNumber(tag, "y"), w, h};
                ok = w > 0 && h > 0;
                if (ok && (rx > 0 || ry > 0)) {
                    s.path.addRoundedRect(r, std::min(rx, w / 2), std::min(ry, h / 2));
                } else if (ok) {
                    s.path.addRect(r);
                }
            } else if (tag.name == "circle" || tag.name == "ellipse") {
                const float cx = attributeNumber(tag, "cx");
                const float cy = attributeNumber(tag, "cy");
                const float rx = attributeNumber(tag, tag.name == "circle" ? "r" : "rx");
                const float ry = attributeNumber(tag, tag.name == "circle" ? "r" : "ry");
                ok = rx > 0 && ry > 0;
                if (ok) {
                    s.path.addEllipse({cx - rx, cy - ry, 2 * rx, 2 * ry});
                }
            } else if (tag.name == "line") {
                s.path.moveTo({attributeNumber(tag, "x1"), attributeNumber(tag, "y1")});
                s.path.lineTo({attributeNumber(tag, "x2"), attributeNumber(tag, "y2")});
            } else {
                ok = points(attribute(tag, "points"), s.path, tag.name == "polygon");
            }
            if (ok && !s.path.empty()) {
                s.transform = st.transform;
                s.fill = st.fill;
                s.stroke = st.stroke;
                s.opacity = st.opacity;
                s.fillOpacity = st.fillOpacity;
                s.strokeOpacity = st.strokeOpacity;
                s.fillRule = st.fillRule;
                s.strokeWidth = st.strokeWidth;
                s.cap = st.cap;
                s.join = st.join;
                s.miterLimit = st.miterLimit;
                if (tag.name == "line" || tag.name == "polyline") {
                    s.fill = {}; // lines have nothing to fill
                }
                image.m_shapes.push_back(std::move(s));
            }
        }
        if (!tag.selfClosing) {
            styles.push_back(st);
        }
    }
    if (!xml.ok || !sawSvg) {
        return Error(ErrorCode::ParseError, "malformed SVG");
    }
    return image;
}

void SvgImage::render(Painter &painter, const RectF &target, Color currentColor) const {
    if (!(m_viewBox.width > 0) || !(m_viewBox.height > 0) || !(target.width > 0) || !(target.height > 0)) {
        return;
    }
    const Transform2D view = Transform2D::translation(-m_viewBox.x, -m_viewBox.y)
                                 .then(Transform2D::scaling(static_cast<double>(target.width) / m_viewBox.width,
                                                            static_cast<double>(target.height) / m_viewBox.height))
                                 .then(Transform2D::translation(target.x, target.y));
    const auto colorOf = [&](const Paint &p, float opacity) {
        Color c = p.kind == Paint::Kind::CurrentColor ? currentColor : p.color;
        c.a *= opacity;
        return c;
    };
    for (const Shape &s : m_shapes) {
        painter.save();
        painter.setTransform(s.transform.then(view), true);
        if (s.opacity < 1) {
            painter.setOpacity(s.opacity);
        }
        if (s.fill.kind != Paint::Kind::None && s.fillOpacity > 0) {
            painter.fillPath(s.path, Brush(colorOf(s.fill, s.fillOpacity)), s.fillRule);
        }
        if (s.stroke.kind != Paint::Kind::None && s.strokeOpacity > 0 && s.strokeWidth > 0) {
            Pen pen(Brush(colorOf(s.stroke, s.strokeOpacity)), s.strokeWidth);
            pen.cap = s.cap;
            pen.join = s.join;
            pen.miterLimit = s.miterLimit;
            painter.strokePath(s.path, pen);
        }
        painter.restore();
    }
}

} // namespace cfw
