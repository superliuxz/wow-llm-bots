// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Standing orders (docs/specs/triggers.md, "The grammar (final, 2026-11-17)"
// and "Decided (2026-11-18)"): `<when> -> <act>, then <act>`, written by the
// brain, watched for by the harness, carried out by the module.
//
// The language is harness/grammar/orders.wlb; its types, parser, printer and
// GBNF are generated from it (grammar/orders.wlb.h, docs/plans/grammar-idl.md).
// This is the harness's small API on top: what plan.cc and the evaluator use.

#ifndef HARNESS_ORDERS_H_
#define HARNESS_ORDERS_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "grammar/orders.wlb.h"

namespace harness {

inline constexpr int kMaxOrders = 7;  // Per plan (`order:` lines).

using grammar::Act;
using grammar::Check;
using grammar::Condition;
using grammar::Order;
using grammar::Place;
using grammar::Thing;
using grammar::When;
using grammar::Who;

// Parse the parts as the grammar writes them (nullopt if `text` isn't one),
// and the text of an order as the grammar writes it (ParseOrder's inverse).
using grammar::FormatOrder;
using grammar::ParseOrder;
using grammar::ParseWhen;
// With `names` (those the call offered), a name that could end in more than
// one place is read as one of them: "use Fire on the ground" uses "Fire on the
// ground" when that was offered, else "Fire" on "the ground".
std::optional<std::vector<Act>> ParseActs(
    std::string_view text,
    const std::vector<std::string>& names = {});

// The GBNF rules for an order (root `orders-Order`, OrdersRuleName) and what it
// uses, with names limited to `names` (what the bot can see or
// knows: units, objects, auras, its spells and items); "" when `names` has no
// usable name, so the plan grammar leaves `order:` lines out.
std::string OrderGrammar(const std::vector<std::string>& names);

}  // namespace harness

#endif  // HARNESS_ORDERS_H_
