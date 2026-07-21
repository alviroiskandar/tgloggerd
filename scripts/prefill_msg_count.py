#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Back-fill users.msg_count and groups.msg_count from the recorded messages,
# so the counters match what the daemon would have accumulated by counting one
# per inserted message row (see DB::bumpMsgCount / upsert*Message).
#
#   users.msg_count  = COUNT(1) of group_messages the user sent (sender_user_id)
#                    + COUNT(1) of private_messages the user sent (sender_id)
#   groups.msg_count = COUNT(1) of group_messages sent to the group (chat_id)
#
# Idempotent: it SETs absolute values, so it is safe to re-run. Stop the daemon
# first so it is not incrementing the same counters concurrently.
#
# Connection settings come from the same TG_DB_* environment variables the
# daemon uses (values are stripped, since .env may pad them for alignment):
#   TG_DB_HOST (default "mysql"), TG_DB_PORT (3306), TG_DB_USER, TG_DB_PASSWORD,
#   TG_DB_NAME (default "tgloggerd").
#
# Requires PyMySQL (pip install pymysql).

import os
import sys

try:
    import pymysql
except ImportError:
    sys.exit("PyMySQL is required: pip install pymysql")


def env(name, default=""):
    return os.environ.get(name, default).strip()


# groups.msg_count = number of group_messages rows per group.
UPDATE_GROUPS = """
UPDATE `groups` g
LEFT JOIN (
    SELECT chat_id, COUNT(1) AS c
    FROM group_messages
    GROUP BY chat_id
) t ON t.chat_id = g.id
SET g.msg_count = COALESCE(t.c, 0)
"""

# users.msg_count = group messages the user sent + private messages they sent.
UPDATE_USERS = """
UPDATE users u
LEFT JOIN (
    SELECT sender_user_id AS uid, COUNT(1) AS c
    FROM group_messages
    WHERE sender_user_id IS NOT NULL
    GROUP BY sender_user_id
) gt ON gt.uid = u.id
LEFT JOIN (
    SELECT sender_id AS uid, COUNT(1) AS c
    FROM private_messages
    WHERE sender_id IS NOT NULL
    GROUP BY sender_id
) pt ON pt.uid = u.id
SET u.msg_count = COALESCE(gt.c, 0) + COALESCE(pt.c, 0)
"""


def main():
    conn = pymysql.connect(
        host=env("TG_DB_HOST", "mysql"),
        port=int(env("TG_DB_PORT", "3306") or "3306"),
        user=env("TG_DB_USER", "tgloggerd"),
        password=env("TG_DB_PASSWORD", "tgloggerd"),
        database=env("TG_DB_NAME", "tgloggerd"),
        charset="utf8mb4",
        autocommit=False,
    )
    try:
        with conn.cursor() as cur:
            print("Prefilling groups.msg_count ...", flush=True)
            cur.execute(UPDATE_GROUPS)
            print(f"  groups rows updated: {cur.rowcount}", flush=True)

            print("Prefilling users.msg_count ...", flush=True)
            cur.execute(UPDATE_USERS)
            print(f"  users rows updated: {cur.rowcount}", flush=True)

        conn.commit()

        # Verify the totals against a direct recount of the source tables.
        with conn.cursor() as cur:
            cur.execute("SELECT COALESCE(SUM(msg_count), 0) FROM `groups`")
            g_sum = cur.fetchone()[0]
            cur.execute("SELECT COUNT(1) FROM group_messages")
            g_msgs = cur.fetchone()[0]

            cur.execute("SELECT COALESCE(SUM(msg_count), 0) FROM users")
            u_sum = cur.fetchone()[0]
            cur.execute(
                "SELECT (SELECT COUNT(1) FROM group_messages"
                "        WHERE sender_user_id IS NOT NULL)"
                "     + (SELECT COUNT(1) FROM private_messages"
                "        WHERE sender_id IS NOT NULL)"
            )
            u_msgs = cur.fetchone()[0]

        print(f"groups: SUM(msg_count)={g_sum} vs group_messages={g_msgs} "
              f"({'OK' if g_sum == g_msgs else 'MISMATCH'})")
        print(f"users:  SUM(msg_count)={u_sum} vs sender rows={u_msgs} "
              f"({'OK' if u_sum == u_msgs else 'MISMATCH'})")
    finally:
        conn.close()


if __name__ == "__main__":
    main()
