"""Durable, day-partitioned console log storage.

Deliberately dumb (design D3): this module stamps and appends, and knows
nothing about boot banners, build ids or faults. Daily files make `since` a
file-selection problem rather than a rotation-offset problem (§6).
"""
import datetime as dt
import os

MARKER = "@@rete-console@@"

DEFAULT_ROOT = os.path.expanduser("~/.local/state/rete/console")
DEFAULT_BUDGET = 2 * 1024 ** 3


class LogStore:
    def __init__(self, root=DEFAULT_ROOT, board="c6", budget_bytes=DEFAULT_BUDGET):
        self.root = root
        self.board = board
        self.budget_bytes = budget_bytes
        os.makedirs(self.root, exist_ok=True)

    def path_for(self, day):
        return os.path.join(self.root, "console-%s-%s.log" % (self.board, day.isoformat()))

    def append(self, text, when):
        if when.tzinfo is None or when.tzinfo.utcoffset(when) is None:
            raise ValueError("timestamp must be timezone-aware; a log spanning "
                             "days needs its zone stated, not assumed (§6)")
        stamp = when.isoformat(timespec="milliseconds")
        with open(self.path_for(when.date()), "a", encoding="utf-8", errors="replace") as fh:
            fh.write("%s %s\n" % (stamp, text))

    def days(self):
        out = []
        prefix = "console-%s-" % self.board
        for name in os.listdir(self.root):
            if not (name.startswith(prefix) and name.endswith(".log")):
                continue
            try:
                out.append(dt.date.fromisoformat(name[len(prefix):-len(".log")]))
            except ValueError:
                continue          # not ours; leave it alone
        return sorted(out)

    def total_bytes(self):
        return sum(os.path.getsize(self.path_for(d)) for d in self.days())

    def prune(self):
        """Delete whole oldest days until under budget. Never the newest day:
        a single wedged day over budget is a fact to report, not a reason to
        destroy the only evidence there is."""
        deleted = []
        days = self.days()
        while len(days) > 1 and self.total_bytes() > self.budget_bytes:
            victim = self.path_for(days.pop(0))
            os.remove(victim)
            deleted.append(victim)
        return deleted
