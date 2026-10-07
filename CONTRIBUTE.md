# Contributing to Clannect Framework

Thank you for helping. This page explains one thing you should know before you
open a pull request: some changes take longer to review than others, and why.

## Every pull request is scanned

A workflow called `cfw-security-guard` runs on every pull request. It checks
the whole repository for malware, for risky C/C++ (code that starts other
programs, loads libraries at run time, or copies memory unsafely), and for
build scripts that download or run things. The result appears as the `verdict`
check on your pull request, and each scan uploads its logs to the run page.

## Changes that take longer to review

Some files in this repository do risky-looking things on purpose. For example,
`modules/cfw-io/src/Process.cpp` starts other programs, because that is its
job. These accepted cases are listed in
[.github/security/allowlist.txt](.github/security/allowlist.txt), one line per
file.

**If your pull request edits a file that is named in the allowlist, or edits
the allowlist itself, expect the review to take longer.** The Clannect team
inspects these changes by hand, line by line, to understand exactly how they
work before approving them. This is true even when the `verdict` check is
green, and even when your change looks small.

The same applies to anything under `.github/`, because those files are the
scanner.

This is not a judgement of you or your code. These are the places where a
mistake or a hidden backdoor would do the most harm to everyone who uses the
framework, so they get the most careful look.

## How to make that review faster

- Check the allowlist before you start, so you know whether your change
  touches a listed file.
- Keep changes to listed files in their own pull request, apart from unrelated
  work. A small, focused change is quicker to inspect.
- In the pull request description, say what the risky call does, why it is
  needed, and where its input comes from.
- Prefer what the framework already offers (for example `cfw::io::Process`)
  to a new call to `system`, `exec`, `CreateProcess`, `dlopen` or
  `LoadLibrary`.

## If the `verdict` check fails

Open the failed run and read its summary: it names the rule, the file and the
line. The full logs are in the run's artifacts (`security-logs-malware`,
`security-logs-sast`, `security-logs-supply-chain`).

You can run two of the scans yourself, from the repository root:

    python3 .github/security/cfw_guard.py cxx
    python3 .github/security/cfw_guard.py supply-chain

If the finding is a mistake in your change, fix the code. If the call is
really needed, add or raise its line in `allowlist.txt` in the same pull
request and explain why in the description. A pull request that changes the
allowlist always gets the longer review described above.

Please do not try to hide a finding, for example by splitting a function name
across a macro. A pull request that does this will be closed.

## Deliberate attempts to add malicious code

Honest mistakes and false alarms are normal, and nobody gets in trouble for
them. This section is about something else: knowingly trying to put malware, a
backdoor or any other hidden harmful code into Clannect Framework.

Doing that on purpose can be a criminal offence. Clannect Framework is built
into other people's software, so harmful code here reaches their computers
too. Depending on where you and the affected users are, laws such as these can
apply:

- **United States:** the Computer Fraud and Abuse Act (18 U.S.C. § 1030).
- **European Union:** Directive 2013/40/EU on attacks against information
  systems, which each member state has put into its own criminal law.
- **United Kingdom:** the Computer Misuse Act 1990.
- **Internationally:** the Council of Europe Convention on Cybercrime (the
  Budapest Convention), which many countries' laws are based on.

If the Clannect team finds a deliberate attempt, we will close the pull
request, block the account from the project, report it to GitHub, and may
report it to law enforcement together with the evidence (the commits, the
pull request and the scan logs).
