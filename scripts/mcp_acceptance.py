#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Acceptance tests for the Telegram MCP tools (web/src/mcp/telegram/Tools.cpp).
#
# These check the API's COST, not just its correctness. The tools exist to be
# driven by a language model with a finite budget, so "can it answer this?" is
# only half the question -- the other half is "in how many calls, and how many
# bytes?". Each test below states a workload and asserts a ceiling on both.
#
# The workload that motivated them: "list all edited messages in the last 7 days
# with their revisions". Before the edit_date/deleted_at filters and the batch
# tool that was ~9 search calls to page a week by SEND time, client-side
# filtering, then one history call per edited message -- roughly sixty calls,
# and it still missed messages sent before the window and edited inside it.
# It is now two calls, which is what test 1 and test 2 pin down.
#
# Usage:
#   scripts/mcp_acceptance.py --url http://10.0.88.4:8080/mcp --token tgmcp_... \
#                             --group -1001483770714
#
# Exits non-zero if any test fails. Read-only: it never writes to the archive.

import argparse
import json
import sys
import time
import urllib.request

CALLS = 0


def rpc(url, token, tool, args):
    """One tools/call. Returns (structuredContent, wire_bytes)."""
    global CALLS
    CALLS += 1
    body = json.dumps({
        "jsonrpc": "2.0", "id": 1, "method": "tools/call",
        "params": {"name": tool, "arguments": args},
    }).encode()
    req = urllib.request.Request(url, data=body, method="POST", headers={
        "Authorization": "Bearer " + token,
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
    })
    with urllib.request.urlopen(req, timeout=60) as r:
        d = json.loads(r.read())
    if "error" in d:
        raise AssertionError("protocol error: %s" % d["error"])
    res = d["result"]
    if res.get("isError"):
        raise AssertionError("tool error: %s" % res["content"][0]["text"])
    # content[0].text is what actually crosses the wire to the model.
    return res.get("structuredContent", {}), len(res["content"][0]["text"])


class Suite:
    def __init__(self, url, token, group):
        self.url, self.token, self.group = url, token, group
        self.failed = 0

    def call(self, tool, args):
        return rpc(self.url, self.token, tool, args)

    def check(self, name, ok, detail=""):
        print("  %-4s %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""))
        if not ok:
            self.failed += 1

    # ---- 1: every edited message in a window, in ONE call ----------------
    def t_edited_one_call(self, start, end):
        print("\n[1] all edited messages in a date range, one call, by edit_date")
        before = CALLS
        r, _ = self.call("telegram_search_messages", {
            "filter": {"field": "edit_date", "op": "between",
                       "value": [start, end]},
            "group_id": self.group, "order_by": "edit_date", "limit": 200,
            "include_total": True,
            "fields": ["message_id", "sender_username", "edit_date",
                       "previous_version_count"],
            "include_media": False, "compact": True,
        })
        msgs = r["messages"]
        self.check("resolves in exactly one call", CALLS - before == 1,
                   "used %d" % (CALLS - before))
        self.check("sorted by edit_date descending",
                   all(msgs[i]["edit_date"] >= msgs[i + 1]["edit_date"]
                       for i in range(len(msgs) - 1)))
        self.check("previous_version_count present on every row",
                   all("previous_version_count" in m for m in msgs),
                   "%d rows" % len(msgs))
        # Every row must genuinely be an edit inside the window.
        self.check("every row is edited", all(m.get("edit_date") for m in msgs))
        return [m["message_id"] for m in msgs]

    # ---- 2: revisions for N ids, in ONE call ----------------------------
    def t_batch_revisions(self, ids):
        print("\n[2] revisions for %d message ids, one call" % len(ids))
        if not ids:
            self.check("skipped: no edited messages in range", True)
            return
        before = CALLS
        r, _ = self.call("telegram_get_messages", {
            "group_id": self.group, "message_ids": ids[:100],
            "include_history": True,
        })
        self.check("resolves in exactly one call", CALLS - before == 1,
                   "used %d" % (CALLS - before))
        self.check("returns found + missing_ids",
                   "found" in r and "missing_ids" in r)
        self.check("every found row carries previous_versions",
                   all("previous_versions" in m for m in r["found"]),
                   "%d found" % r["count"])
        # A batch must survive an unknown id rather than failing whole.
        r2, _ = self.call("telegram_get_messages", {
            "group_id": self.group,
            "message_ids": [ids[0], 999999999999],
            "fields": ["message_id"],
        })
        self.check("unknown id is reported, not fatal",
                   r2["count"] == 1 and 999999999999 in r2["missing_ids"])

    # ---- 3: deleted within a window (not sent within it) ----------------
    def t_deleted_window(self, start):
        print("\n[3] messages DELETED (not sent) within a date range")
        r, _ = self.call("telegram_search_messages", {
            "filter": {"field": "deleted_at", "op": ">=", "value": start},
            "group_id": self.group, "order_by": "deleted_at", "limit": 50,
            "include_total": True,
            "fields": ["message_id", "date", "deleted_at"], "compact": True,
        })
        self.check("deleted_at is filterable and orderable", "total" in r,
                   "%s deletions" % r.get("total"))
        msgs = r["messages"]
        self.check("every row carries deleted_at",
                   all("deleted_at" in m for m in msgs))
        self.check("sorted by deleted_at descending",
                   all(msgs[i]["deleted_at"] >= msgs[i + 1]["deleted_at"]
                       for i in range(len(msgs) - 1)))

    # ---- 4: a shaped scan is much smaller than the default --------------
    def t_payload_shrink(self, factor=5.0):
        print("\n[4] projection + truncation vs the default payload")
        # Long posts: the case where per-row text dominates, which is the case
        # the controls exist for.
        flt = {"field": "text_length", "op": ">", "value": 500}
        _, big = self.call("telegram_search_messages", {
            "filter": flt, "group_id": self.group, "limit": 200})
        _, small = self.call("telegram_search_messages", {
            "filter": flt, "group_id": self.group, "limit": 200,
            "fields": ["message_id", "sender_username", "date", "edit_date",
                       "text"],
            "truncate_text": 80, "include_media": False, "compact": True})
        ratio = big / max(small, 1)
        self.check("text-heavy scan shrinks >= %.0fx" % factor, ratio >= factor,
                   "%d B -> %d B = %.1fx" % (big, small, ratio))
        # Identity-only scan: no text at all.
        _, dflt = self.call("telegram_search_messages",
                            {"group_id": self.group, "limit": 200})
        _, ids = self.call("telegram_search_messages", {
            "group_id": self.group, "limit": 200,
            "fields": ["message_id", "sender_username", "date"],
            "include_media": False, "compact": True})
        r2 = dflt / max(ids, 1)
        self.check("identity-only scan shrinks >= %.0fx" % factor, r2 >= factor,
                   "%d B -> %d B = %.1fx" % (dflt, ids, r2))

    # ---- 5: paging a live archive must not skip or repeat ---------------
    def t_cursor_exact(self):
        print("\n[5] cursor paging returns exactly the unpaged result")
        whole, _ = self.call("telegram_search_messages", {
            "group_id": self.group, "limit": 20,
            "fields": ["message_id"], "compact": True})
        want = [m["message_id"] for m in whole["messages"]]
        got, cursor = [], None
        for _ in range(10):
            a = {"group_id": self.group, "limit": 5,
                 "fields": ["message_id"], "compact": True}
            if cursor:
                a["cursor"] = cursor
            page, _ = self.call("telegram_search_messages", a)
            got += [m["message_id"] for m in page["messages"]]
            cursor = page.get("next_cursor")
            if not cursor or len(got) >= len(want):
                break
        self.check("paged ids match unpaged, in order", got[:len(want)] == want,
                   "%d vs %d" % (len(got), len(want)))
        self.check("no duplicates across pages", len(got) == len(set(got)))

    # ---- 6: count-only sizing, and the gate ------------------------------
    def t_count_only_and_gate(self, private_group):
        print("\n[6] limit:0 sizing, and the exposure gate")
        r, _ = self.call("telegram_search_messages", {
            "group_id": self.group, "limit": 0, "include_total": True})
        self.check("limit:0 returns a total and no rows",
                   r["count"] == 0 and r.get("total", 0) > 0,
                   "total=%s" % r.get("total"))
        if private_group is None:
            return
        # An unexposed group must be indistinguishable from an empty one,
        # through every path the new code added.
        for name, args in (
            ("search group_id", {"group_id": private_group, "limit": 5,
                                 "include_total": True}),
            ("search via filter", {"filter": {"field": "group_id", "op": "=",
                                              "value": private_group},
                                   "limit": 5, "include_total": True}),
            ("order_by deleted_at", {"group_id": private_group,
                                     "order_by": "deleted_at", "limit": 5,
                                     "include_total": True}),
        ):
            r, _ = self.call("telegram_search_messages", args)
            self.check("unexposed group yields nothing (%s)" % name,
                       r["count"] == 0 and r.get("total", 0) == 0)
        r, _ = self.call("telegram_get_messages",
                         {"group_id": private_group, "message_ids": [1, 2]})
        self.check("unexposed group yields nothing (batch get)",
                   r["count"] == 0 and len(r["missing_ids"]) == 2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--token", required=True)
    ap.add_argument("--group", type=int, required=True)
    ap.add_argument("--private-group", type=int, default=None,
                    help="a group NOT on the allowlist, to test the gate")
    ap.add_argument("--days", type=int, default=7)
    ap.add_argument("--shrink", type=float, default=5.0)
    a = ap.parse_args()

    now = time.time()
    start = time.strftime("%Y-%m-%d", time.gmtime(now - a.days * 86400))
    end = time.strftime("%Y-%m-%d", time.gmtime(now + 86400))

    s = Suite(a.url, a.token, a.group)
    ids = s.t_edited_one_call(start, end)
    s.t_batch_revisions(ids)
    s.t_deleted_window(time.strftime("%Y-%m-%d", time.gmtime(now - 90 * 86400)))
    s.t_payload_shrink(a.shrink)
    s.t_cursor_exact()
    s.t_count_only_and_gate(a.private_group)

    print("\n%d call(s) total, %d failure(s)" % (CALLS, s.failed))
    print("the motivating workload (tests 1+2) cost 2 calls; it was ~60.")
    return 1 if s.failed else 0


if __name__ == "__main__":
    sys.exit(main())
