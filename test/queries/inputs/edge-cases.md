---
"type": Metric
'title': Edge Cases
description: >-
  A synthetic concept exercising every
  query in queries/okf.
tags: [queries, fixtures]
okf_version: "0.2"
usage_count: 1240
ratio: 0.25
enabled: true
legacy_flag: yes
nothing: ~
stale_after: 2026-12-31T00:00:00Z
generated: { by: agent, at: '2026-06-30T14:00:00+00:00' }
sources:
- id: flow-source
  resource: tables/orders.md
- { id: inline-source, resource: all queries in BigQuery project X, usage_count: 3 }
nested:
  title: not the document title
---

# Definition

Revenue per [the policy](/policies/revenue.md), [a sibling](./sibling.md),
[a parent](../tables/orders.md), [a bare path](orders.md), [the web](https://example.com)
and [a fragment](#schema).[^flow-source] See <https://example.com/auto>.[^inline-source]

![chart](charts/revenue.png)

[ref]: /metrics/revenue.md

# Schema

| Column | Type |
|--------|------|
| `amount` | NUMERIC |

# computation

    SELECT SUM(amount)
    FROM orders

## 2026-07-01

Not a log entry by position alone: it is still a `## YYYY-MM-DD` section.

[^flow-source]: Orders
[^inline-source]: Everything
