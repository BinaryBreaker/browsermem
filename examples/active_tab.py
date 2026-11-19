"""Print every open browser window and the URL of each window's active tab."""

import browsermem

print(f"browsermem {browsermem.__version__}\n")

for tab in browsermem.get_all_tabs():
    print(f"{tab.browser_name:<15} {tab.title}")
    print(f"{'':<15} {tab.url or '<unresolved>'}  (confidence {tab.confidence:.2f})\n")

active = browsermem.get_active_tab()
print("Active tab:", active.url if active else "no browser open")
