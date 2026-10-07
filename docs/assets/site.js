// Clannect Framework Docs: the header, sidebar, search, "In this article" and
// code blocks around each page's <main>. No dependencies, no build step, and
// it works from a file:// URL. A page is plain HTML: to add one, write it,
// then list it in NAV below.

(function () {
    "use strict";

    var VERSION = "0.2.1";
    var REPOSITORY = "https://github.com/Clannect/ClannectFramework";

    // The sidebar, the search index and the previous/next links.
    var NAV = [
        {
            title: "Get started",
            pages: [
                {
                    id: "get-started/build",
                    title: "Build and run the tests",
                    summary: "Clone, configure with a CMake preset, build, and run the test suite.",
                    keywords: "install cmake ninja compiler gcc clang mingw linux windows libx11 ctest gallery",
                },
                {
                    id: "get-started/use-in-your-project",
                    title: "Use CFW in your project",
                    summary: "Add CFW with FetchContent, or install a release and use find_package.",
                    keywords: "fetchcontent find_package installer online offline release target_link_libraries prefix",
                },
                {
                    id: "get-started/hello-window",
                    title: "Hello, window",
                    summary: "A window with a label and a button, explained line by line.",
                    keywords: "uiwindow theme stack label button clicked first program example tutorial",
                },
            ],
        },
        {
            title: "Concepts",
            pages: [
                {
                    id: "concepts/modules",
                    title: "Modules and platforms",
                    summary: "The ten modules, how they are layered, and where CFW runs.",
                    keywords: "cfw-core cfw-io cfw-net cfw-image cfw-audio cfw-text cfw-gfx cfw-platform cfw-ui cfw-app windows linux macos headless server",
                },
                {
                    id: "concepts/errors",
                    title: "Errors and Result",
                    summary: "CFW does not throw for errors: fallible calls return Result<T>.",
                    keywords: "result error errorcode exceptions value ok valueOr describe with success nodiscard",
                },
                {
                    id: "concepts/signals",
                    title: "Signals",
                    summary: "Typed notifications in plain C++, and how connections end.",
                    keywords: "signal slot connect emit connection scopedconnection signalowner disconnect callback event",
                },
            ],
        },
        {
            title: "Guides",
            pages: [
                {
                    id: "guides/sound",
                    title: "Play sound",
                    summary: "Decode WAV, Ogg Vorbis, MP3 and FLAC, and play through the system's device.",
                    keywords: "audio cfw-audio audiodevice decodeAudio audiostream resample resampler wasapi alsa coreaudio music clip mixer",
                },
                {
                    id: "guides/input",
                    title: "Read input",
                    summary: "Relative mouse mode, keys by layout and by position, touch, pen and gamepads.",
                    keywords: "keyboard mouse pointer relative camera wasd physicalKey touch pen gamepad rumble controller wayland",
                },
            ],
        },
        {
            title: "Reference",
            pages: [
                {
                    id: "reference/build-presets",
                    title: "Build presets",
                    summary: "The CMake presets, the sanitizer builds, and cross-compiling for Windows.",
                    keywords: "debug release asan tsan ubsan fuzz libfuzzer sanitizer wine mingw cross toolchain",
                },
            ],
        },
        {
            title: "Project",
            pages: [
                {
                    id: "project/testing",
                    title: "How CFW is tested",
                    summary: "Reference outputs, fuzzing, sanitizers, real system clients and soak tests.",
                    keywords: "tests oracle pngsuite harfbuzz freetype ffmpeg libjpeg libwebp fuzz asan soak ci",
                },
                {
                    id: "project/security",
                    title: "Security checks",
                    summary: "The security guard that scans every pull request, and what the allowlist is.",
                    keywords: "security guard malware clamav flawfinder bandit allowlist verdict supply chain pull request review backdoor",
                },
            ],
        },
    ];

    var doc = document;
    var root = doc.documentElement.getAttribute("data-root") || "";
    var pageId = doc.body.getAttribute("data-page") || "";
    var main = doc.querySelector("main");
    var isHome = pageId === "";

    function el(tag, attributes, children) {
        var node = doc.createElement(tag);
        Object.keys(attributes || {}).forEach(function (name) {
            if (name === "text") {
                node.textContent = attributes[name];
            } else if (name === "html") {
                node.innerHTML = attributes[name];
            } else {
                node.setAttribute(name, attributes[name]);
            }
        });
        (children || []).forEach(function (child) {
            if (child) {
                node.appendChild(child);
            }
        });
        return node;
    }

    function href(id) {
        return root + id + ".html";
    }

    var flat = [];
    NAV.forEach(function (group) {
        group.pages.forEach(function (page) {
            flat.push({ group: group.title, page: page });
        });
    });
    var position = flat.findIndex(function (entry) {
        return entry.page.id === pageId;
    });

    // ---- Theme ----

    function currentTheme() {
        var set = doc.documentElement.getAttribute("data-theme");
        if (set) {
            return set;
        }
        return window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
    }

    function toggleTheme() {
        var next = currentTheme() === "dark" ? "light" : "dark";
        doc.documentElement.setAttribute("data-theme", next);
        try {
            localStorage.setItem("cfw-docs-theme", next);
        } catch (error) {
            // Private windows: the choice lasts for this page only.
        }
    }

    // ---- Header ----

    var ICON_THEME =
        '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M8 1a7 7 0 1 0 0 14A7 7 0 0 0 8 1Zm0 12.5v-11a5.5 5.5 0 0 1 0 11Z"/></svg>';
    var ICON_MENU =
        '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M1 3h14v1.5H1V3Zm0 4.25h14v1.5H1v-1.5Zm0 4.25h14V13H1v-1.5Z"/></svg>';
    var ICON_GITHUB =
        '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M8 0a8 8 0 0 0-2.53 15.59c.4.07.55-.17.55-.38v-1.33c-2.23.48-2.7-1.07-2.7-1.07-.36-.93-.89-1.17-.89-1.17-.73-.5.05-.49.05-.49.8.06 1.23.83 1.23.83.72 1.22 1.88.87 2.33.66.07-.52.28-.87.5-1.07-1.77-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82a7.6 7.6 0 0 1 4 0c1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48v2.19c0 .21.15.46.55.38A8 8 0 0 0 8 0Z"/></svg>';

    var searchInput = el("input", {
        type: "search",
        placeholder: "Search the docs",
        "aria-label": "Search the docs",
        autocomplete: "off",
        spellcheck: "false",
    });
    var searchResults = el("ul", { class: "search-results", hidden: "" });
    var menuButton = el("button", { class: "icon-button menu-button", type: "button", "aria-label": "Menu", html: ICON_MENU });
    var themeButton = el("button", {
        class: "icon-button",
        type: "button",
        "aria-label": "Switch between light and dark",
        title: "Switch between light and dark",
        html: ICON_THEME,
    });
    themeButton.addEventListener("click", toggleTheme);

    var header = el("header", { class: "site-header" }, [
        isHome ? null : menuButton,
        el("a", { class: "brand", href: root + "index.html" }, [
            el("span", { class: "brand-mark", text: "CFW", "aria-hidden": "true" }),
            el("span", { html: 'Clannect Framework <span class="brand-docs">Docs</span>' }),
        ]),
        el("span", { class: "version", text: "v" + VERSION, title: "These pages describe version " + VERSION }),
        el("span", { class: "header-space" }),
        el("div", { class: "search" }, [searchInput, el("kbd", { text: "/" }), searchResults]),
        themeButton,
        el("a", { class: "icon-button", href: REPOSITORY, "aria-label": "The repository on GitHub", title: "GitHub", html: ICON_GITHUB }),
    ]);
    doc.body.insertBefore(header, doc.body.firstChild);

    // ---- Search ----

    var activeResult = -1;

    function search(query) {
        var terms = query.toLowerCase().split(/\s+/).filter(Boolean);
        if (!terms.length) {
            return [];
        }
        return flat
            .map(function (entry) {
                var title = entry.page.title.toLowerCase();
                var rest = (entry.page.summary + " " + entry.page.keywords + " " + entry.group).toLowerCase();
                var score = 0;
                for (var i = 0; i < terms.length; i++) {
                    if (title.indexOf(terms[i]) >= 0) {
                        score += 3;
                    } else if (rest.indexOf(terms[i]) >= 0) {
                        score += 1;
                    } else {
                        return null;
                    }
                }
                return { entry: entry, score: score };
            })
            .filter(Boolean)
            .sort(function (a, b) {
                return b.score - a.score;
            });
    }

    function showResults() {
        var found = search(searchInput.value);
        searchResults.textContent = "";
        activeResult = -1;
        if (!searchInput.value.trim()) {
            searchResults.hidden = true;
            return;
        }
        if (!found.length) {
            searchResults.appendChild(el("li", { class: "no-results", text: "Nothing found. Try a module name, such as cfw-net." }));
        }
        found.forEach(function (match) {
            searchResults.appendChild(
                el("li", {}, [
                    el("a", { href: href(match.entry.page.id) }, [
                        el("div", { class: "result-group", text: match.entry.group }),
                        el("div", { class: "result-title", text: match.entry.page.title }),
                        el("div", { class: "result-summary", text: match.entry.page.summary }),
                    ]),
                ])
            );
        });
        searchResults.hidden = false;
    }

    function moveResult(step) {
        var links = searchResults.querySelectorAll("a");
        if (!links.length) {
            return;
        }
        if (activeResult >= 0) {
            links[activeResult].classList.remove("active");
        }
        activeResult = (activeResult + step + links.length) % links.length;
        links[activeResult].classList.add("active");
        links[activeResult].scrollIntoView({ block: "nearest" });
    }

    searchInput.addEventListener("input", showResults);
    searchInput.addEventListener("focus", showResults);
    searchInput.addEventListener("keydown", function (event) {
        if (event.key === "ArrowDown") {
            event.preventDefault();
            moveResult(1);
        } else if (event.key === "ArrowUp") {
            event.preventDefault();
            moveResult(-1);
        } else if (event.key === "Enter") {
            var links = searchResults.querySelectorAll("a");
            var chosen = links[activeResult >= 0 ? activeResult : 0];
            if (chosen) {
                window.location.href = chosen.href;
            }
        } else if (event.key === "Escape") {
            searchInput.value = "";
            searchResults.hidden = true;
            searchInput.blur();
        }
    });
    doc.addEventListener("click", function (event) {
        if (!header.querySelector(".search").contains(event.target)) {
            searchResults.hidden = true;
        }
    });
    doc.addEventListener("keydown", function (event) {
        var typing = /^(INPUT|TEXTAREA|SELECT)$/.test(event.target.tagName) || event.target.isContentEditable;
        if (event.key === "/" && !typing && !event.ctrlKey && !event.metaKey && !event.altKey) {
            event.preventDefault();
            searchInput.focus();
        }
    });

    // ---- Layout: sidebar, article, "In this article" ----

    var layout = el("div", { class: "layout" });
    doc.body.insertBefore(layout, main);
    if (isHome) {
        doc.body.classList.add("home");
    }
    main.classList.add("article");

    if (!isHome) {
        var sidebar = el("nav", { class: "sidebar", "aria-label": "Documentation" }, [
            el("a", { class: "sidebar-title", href: root + "index.html", text: "Clannect Framework" }),
        ]);
        NAV.forEach(function (group) {
            sidebar.appendChild(el("div", { class: "sidebar-group", text: group.title }));
            var list = el("ul");
            group.pages.forEach(function (page) {
                var link = el("a", { href: href(page.id), text: page.title });
                if (page.id === pageId) {
                    link.setAttribute("aria-current", "page");
                }
                list.appendChild(el("li", {}, [link]));
            });
            sidebar.appendChild(list);
        });
        layout.appendChild(sidebar);
        menuButton.addEventListener("click", function () {
            doc.body.classList.toggle("menu-open");
        });
        doc.addEventListener("click", function (event) {
            if (doc.body.classList.contains("menu-open") && !sidebar.contains(event.target) && !menuButton.contains(event.target)) {
                doc.body.classList.remove("menu-open");
            }
        });
    }

    layout.appendChild(main);

    if (!isHome && position >= 0) {
        main.insertBefore(
            el("ol", { class: "breadcrumbs", "aria-label": "Breadcrumbs" }, [
                el("li", {}, [el("a", { href: root + "index.html", text: "Docs" })]),
                el("li", { text: flat[position].group }),
                el("li", { text: flat[position].page.title }),
            ]),
            main.firstChild
        );
    }

    // Headings get an id and a link; the h2 and h3 make "In this article".
    if (!isHome) {
        var used = {};
        var headings = [].slice.call(main.querySelectorAll("h2, h3"));
        headings.forEach(function (heading) {
            if (!heading.id) {
                var slug = heading.textContent.trim().toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "section";
                var unique = slug;
                for (var n = 2; used[unique] || doc.getElementById(unique); n++) {
                    unique = slug + "-" + n;
                }
                heading.id = unique;
            }
            used[heading.id] = true;
            heading.appendChild(el("a", { class: "anchor", href: "#" + heading.id, "aria-label": "Link to this section", text: "#" }));
        });

        var toc = el("aside", { class: "toc", "aria-label": "In this article" });
        if (headings.length > 1) {
            var tocList = el("ul");
            var tocLinks = {};
            headings.forEach(function (heading) {
                var link = el("a", {
                    href: "#" + heading.id,
                    class: "depth-" + heading.tagName.charAt(1),
                    text: heading.firstChild.textContent,
                });
                tocLinks[heading.id] = link;
                tocList.appendChild(el("li", {}, [link]));
            });
            toc.appendChild(el("div", { class: "toc-title", text: "In this article" }));
            toc.appendChild(tocList);

            if ("IntersectionObserver" in window) {
                var visible = {};
                var observer = new IntersectionObserver(
                    function (entries) {
                        entries.forEach(function (entry) {
                            visible[entry.target.id] = entry.isIntersecting;
                        });
                        var current = headings.filter(function (heading) {
                            return visible[heading.id];
                        })[0];
                        if (current) {
                            Object.keys(tocLinks).forEach(function (id) {
                                tocLinks[id].classList.toggle("active", id === current.id);
                            });
                        }
                    },
                    { rootMargin: "-64px 0px -65% 0px" }
                );
                headings.forEach(function (heading) {
                    observer.observe(heading);
                });
            }
        }
        layout.appendChild(toc);
    }

    // ---- Tables scroll sideways on narrow screens ----

    [].slice.call(main.querySelectorAll("table")).forEach(function (table) {
        var wrap = el("div", { class: "table-wrap" });
        table.parentNode.insertBefore(wrap, table);
        wrap.appendChild(table);
    });

    // ---- Code blocks: a language label, a copy button, and colours ----

    var LANGUAGES = {
        cpp: {
            label: "C++",
            pattern:
                /(\/\/[^\n]*|\/\*[\s\S]*?\*\/)|("(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])+')|(^[ \t]*#[ \t]*\w+)|\b(alignas|alignof|auto|bool|break|case|catch|char|class|concept|const|constexpr|const_cast|continue|co_await|co_return|co_yield|decltype|default|delete|do|double|dynamic_cast|else|enum|explicit|extern|false|float|for|friend|if|inline|int|long|mutable|namespace|new|noexcept|nullptr|operator|private|protected|public|reinterpret_cast|requires|return|short|signed|sizeof|static|static_cast|struct|switch|template|this|throw|true|try|typedef|typename|union|unsigned|using|virtual|void|volatile|while)\b|\b(\d[\d.']*[fFuUlL]*)\b/gm,
            kinds: ["comment", "string", "preproc", "keyword", "number"],
        },
        cmake: {
            label: "CMake",
            pattern: /(#[^\n]*)|("(?:\\.|[^"\\])*")|(\$\{[^}]*\})|\b([a-zA-Z_]\w*)(?=\s*\()|\b([A-Z][A-Z_]{2,})\b/g,
            kinds: ["comment", "string", "preproc", "keyword", "number"],
        },
        sh: {
            label: "Shell",
            pattern: /((?:^|\s)#[^\n]*)|("(?:\\.|[^"\\])*"|'[^']*')|(\$\{?\w+\}?)|(\s--?[a-zA-Z][\w-]*)/gm,
            kinds: ["comment", "string", "preproc", "keyword"],
        },
        text: { label: "Text" },
    };

    function escapeHtml(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    function highlight(text, language) {
        if (!language.pattern) {
            return escapeHtml(text);
        }
        var out = "";
        var last = 0;
        language.pattern.lastIndex = 0;
        var match;
        while ((match = language.pattern.exec(text))) {
            if (match[0] === "") {
                language.pattern.lastIndex++;
                continue;
            }
            var kind = "";
            for (var group = 1; group < match.length; group++) {
                if (match[group] !== undefined) {
                    kind = language.kinds[group - 1];
                    break;
                }
            }
            out += escapeHtml(text.slice(last, match.index));
            out += '<span class="tok-' + kind + '">' + escapeHtml(match[0]) + "</span>";
            last = match.index + match[0].length;
        }
        return out + escapeHtml(text.slice(last));
    }

    [].slice.call(main.querySelectorAll("pre > code")).forEach(function (code) {
        var pre = code.parentNode;
        var name = (code.className.match(/language-(\w+)/) || [])[1] || "text";
        var language = LANGUAGES[name] || LANGUAGES.text;
        var source = code.textContent.replace(/\n$/, "");
        code.innerHTML = highlight(source, language);

        var copy = el("button", { class: "copy-button", type: "button", text: "Copy" });
        copy.addEventListener("click", function () {
            var done = function () {
                copy.textContent = "Copied";
                setTimeout(function () {
                    copy.textContent = "Copy";
                }, 1500);
            };
            if (navigator.clipboard && navigator.clipboard.writeText) {
                navigator.clipboard.writeText(source).then(done, function () {});
            } else {
                var area = el("textarea");
                area.value = source;
                doc.body.appendChild(area);
                area.select();
                try {
                    doc.execCommand("copy");
                    done();
                } catch (error) {
                    // Copying is not available here; the text can still be selected.
                }
                doc.body.removeChild(area);
            }
        });

        var block = el("div", { class: "code-block" }, [el("div", { class: "code-bar" }, [el("span", { text: language.label }), copy])]);
        pre.parentNode.insertBefore(block, pre);
        block.appendChild(pre);
    });

    // ---- Previous and next, and the footer ----

    if (!isHome && position >= 0) {
        var previous = flat[position - 1];
        var next = flat[position + 1];
        main.appendChild(
            el("nav", { class: "page-nav", "aria-label": "Previous and next page" }, [
                previous
                    ? el("a", { class: "previous", href: href(previous.page.id) }, [
                          el("small", { text: "Previous" }),
                          el("span", { text: previous.page.title }),
                      ])
                    : null,
                next
                    ? el("a", { class: "next", href: href(next.page.id) }, [el("small", { text: "Next" }), el("span", { text: next.page.title })])
                    : null,
            ])
        );
    }

    var source = REPOSITORY + "/blob/main/docs/" + (isHome ? "index" : pageId) + ".html";
    main.appendChild(
        el("footer", { class: "page-footer" }, [
            el("span", {
                html:
                    'Clannect Framework is available under the <a href="' +
                    REPOSITORY +
                    '/blob/main/license.md">Open Use License v1.1</a>.',
            }),
            el("span", {
                html:
                    'Something wrong or missing? <a href="' +
                    source +
                    '">See this page\'s source</a> or <a href="' +
                    REPOSITORY +
                    '/issues">open an issue</a>.',
            }),
        ])
    );
})();
