# Security policy

## Supported versions

Security fixes are made for the latest released version.

## Reporting a vulnerability

Please **don't** open a public issue for security problems. Report them privately through
[GitHub Security Advisories](https://github.com/BinaryBreaker/browsermem/security/advisories/new).
You should get a response within a few days.

## Scope and design

browsermem only **reads** memory, and only of processes the current user is allowed to
open (`PROCESS_VM_READ | PROCESS_QUERY_INFORMATION`). It never writes to or injects into
another process, never escalates privileges, and never sends data anywhere. Everything it
reads stays inside your Python process.

Note that a URL can contain sensitive data, such as session tokens in query strings. If
your application stores or transmits URLs from browsermem, treat them as you would any
browsing history.

## Responsible use

Use browsermem on machines and accounts you own or administer, or with the informed
consent of the people being monitored. Covertly monitoring someone else's browsing may be
illegal in your jurisdiction.
